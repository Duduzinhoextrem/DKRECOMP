/* Windows Vulkan presentation backend. No game data, guest CPU state or system SDK installation is required.
 * Vulkan declarations come from the pinned Khronos headers (tools/fetch_gfx_deps.py); entry points are loaded
 * from the system loader and the selected device. This is the base for the NVN host front end. */
#include "vk_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int dkr_vk_fail(DkrGfx *g, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g->error, sizeof g->error, fmt, args);
    va_end(args);
    return -1;
}

#define fail dkr_vk_fail

/* Window thread only. Borderless covers the monitor the window is on; windowed restores the previous rectangle. */
static void set_borderless(DkrGfx *g, int on) {
    HWND w = g->window;
    if (on) {
        GetWindowRect(w, &g->windowed);
        MONITORINFO mi = {.cbSize = sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(w, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrW(w, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        RECT r = g->windowed;
        SetWindowPos(w, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    g->borderless = on;
    g->dirty = 1;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    DkrGfx *g = (DkrGfx *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        g = (DkrGfx *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)g);
    }
    if (g) {
        if (message == WM_CLOSE) { g->closed = 1; return 0; }
        if (message == WM_SIZE) g->dirty = 1;
        if (message == WM_ERASEBKGND) return 1;
        /* Alt+Enter or F11: borderless full screen on the window's monitor and back. */
        if ((message == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29))) || (message == WM_KEYDOWN && wp == VK_F11 && !(lp & (1 << 30)))) {
            set_borderless(g, !g->borderless);
            return 0;
        }
        if (message == WM_SYSCHAR && wp == '\r') return 0; /* no beep for Alt+Enter */
    }
    return DefWindowProcW(window, message, wp, lp);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    DkrGfx *g = user;
    int validation = !!(type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT);
    if (validation && (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) InterlockedIncrement(&g->validation_errors);
    fprintf(stderr, "vulkan [%s]: %s\n", validation ? "validation" : "general", data->pMessage);
    return VK_FALSE;
}

static int create_instance(DkrGfx *g, int validation) {
    g->loader = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g->loader) return fail(g, "cannot load the system Vulkan loader (Windows error %lu)", GetLastError());
    g->get_instance_proc = (PFN_vkGetInstanceProcAddr)GetProcAddress(g->loader, "vkGetInstanceProcAddr");
    if (!g->get_instance_proc) return fail(g, "Vulkan loader has no vkGetInstanceProcAddr");
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)g->get_instance_proc(NULL, "vkCreateInstance");
    PFN_vkEnumerateInstanceLayerProperties layers_fn =
        (PFN_vkEnumerateInstanceLayerProperties)g->get_instance_proc(NULL, "vkEnumerateInstanceLayerProperties");
    PFN_vkEnumerateInstanceExtensionProperties extensions_fn =
        (PFN_vkEnumerateInstanceExtensionProperties)g->get_instance_proc(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!create || !layers_fn || !extensions_fn) return fail(g, "Vulkan loader is missing global entry points");
    uint32_t count = 0;
    VK_TRY(g, layers_fn(&count, NULL));
    VkLayerProperties *layers = calloc(count ? count : 1, sizeof *layers);
    if (!layers) return fail(g, "out of host memory reading Vulkan layers");
    VkResult result = layers_fn(&count, layers);
    int available = 0;
    for (uint32_t i = 0; result == VK_SUCCESS && i < count; i++)
        if (!strcmp(layers[i].layerName, "VK_LAYER_KHRONOS_validation")) available = 1;
    free(layers);
    if (result != VK_SUCCESS) return fail(g, "cannot enumerate Vulkan layers: %d", result);
    if (validation > 0 && !available) return fail(g, "VK_LAYER_KHRONOS_validation was requested but is not available");
    g->validation = validation != 0 && available;
    VK_TRY(g, extensions_fn(NULL, &count, NULL));
    VkExtensionProperties *properties = calloc(count ? count : 1, sizeof *properties);
    if (!properties) return fail(g, "out of host memory reading Vulkan extensions");
    result = extensions_fn(NULL, &count, properties);
    int has_debug = 0, has_surface = 0, has_win32 = 0;
    for (uint32_t i = 0; result == VK_SUCCESS && i < count; i++) {
        if (!strcmp(properties[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) has_debug = 1;
        if (!strcmp(properties[i].extensionName, VK_KHR_SURFACE_EXTENSION_NAME)) has_surface = 1;
        if (!strcmp(properties[i].extensionName, VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) has_win32 = 1;
    }
    free(properties);
    if (result != VK_SUCCESS || !has_surface || !has_win32) return fail(g, "Vulkan Win32 surface extensions are unavailable");
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                                VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const char *layer = "VK_LAYER_KHRONOS_validation";
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "DKRecomp",
        .applicationVersion = 1, .pEngineName = "DKRecomp host", .engineVersion = 1, .apiVersion = VK_API_VERSION_1_3};
    VkDebugUtilsMessengerCreateInfoEXT debug = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT, .pfnUserCallback = debug_message, .pUserData = g};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
        .enabledExtensionCount = has_debug ? 3u : 2u, .ppEnabledExtensionNames = extensions,
        .enabledLayerCount = g->validation ? 1u : 0u, .ppEnabledLayerNames = &layer, .pNext = has_debug ? &debug : NULL};
    VK_TRY(g, create(&info, NULL, &g->instance));
#define LOAD_INSTANCE(name) do { \
    g->vk##name = (PFN_vk##name)g->get_instance_proc(g->instance, "vk" #name); \
    if (!g->vk##name) return fail(g, "missing Vulkan instance entry point vk%s", #name); \
} while (0);
    INSTANCE_PROCS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    if (has_debug) {
        PFN_vkCreateDebugUtilsMessengerEXT create_debug =
            (PFN_vkCreateDebugUtilsMessengerEXT)g->get_instance_proc(g->instance, "vkCreateDebugUtilsMessengerEXT");
        g->destroy_debug = (PFN_vkDestroyDebugUtilsMessengerEXT)g->get_instance_proc(g->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (create_debug && g->destroy_debug) VK_TRY(g, create_debug(g->instance, &debug, NULL, &g->debug));
    }
    fprintf(stderr, "vulkan: validation layer %s\n", g->validation ? "enabled" : "not enabled");
    return 0;
}

/* A window's messages can only be retrieved by the thread that created it. The game presents from a different
 * guest thread than the one that initializes graphics, so pumping from the presenter never saw the window's
 * input: after a click Windows waited 5 s and replaced it with a frozen "not responding" ghost. The window
 * therefore lives on a small thread of its own that only creates it and runs its message loop. */
#define WM_DKR_DESTROY (WM_APP + 1)
#define WM_DKR_TITLE (WM_APP + 2)
typedef struct WindowStart {
    DkrGfx *g;
    const DkrGfxConfig *config;
    HANDLE ready;
    DWORD error;
    const char *what;
} WindowStart;

static DWORD WINAPI window_thread(void *arg) {
    WindowStart *start = arg;
    DkrGfx *g = start->g;
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = window_proc, .hInstance = instance,
        .hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)), .lpszClassName = L"DKRecomp.Vulkan.Window"};
    wchar_t title[256];
    RECT rect = {0, 0, (LONG)start->config->width, (LONG)start->config->height};
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) start->what = "RegisterClassExW";
    else if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, start->config->title ? start->config->title : "DKRecomp", -1, title, 256))
        start->what = "the window title";
    else if (!AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE)) start->what = "AdjustWindowRect";
    else if (!(g->window = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, instance, g))) start->what = "CreateWindowExW";
    if (start->what) { start->error = GetLastError(); SetEvent(start->ready); return 1; }
    if (start->config->visible) {
        ShowWindow(g->window, SW_SHOW);
        if (start->config->borderless) set_borderless(g, 1);
    }
    SetEvent(start->ready); /* `start` belongs to the creating thread and is not touched after this */
    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if (message.message == WM_DKR_DESTROY) { DestroyWindow(g->window); PostQuitMessage(0); continue; }
        if (message.message == WM_DKR_TITLE) {
            wchar_t *text = (wchar_t *)message.lParam;
            SetWindowTextW(g->window, text);
            free(text);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

static int create_window(DkrGfx *g, const DkrGfxConfig *config) {
    HINSTANCE instance = GetModuleHandleW(NULL);
    WindowStart start = {g, config, CreateEventW(NULL, TRUE, FALSE, NULL), 0, NULL};
    if (!start.ready) return fail(g, "CreateEventW failed: %lu", GetLastError());
    g->window_thread = CreateThread(NULL, 0, window_thread, &start, 0, NULL);
    if (!g->window_thread) { CloseHandle(start.ready); return fail(g, "cannot start the window thread: %lu", GetLastError()); }
    HANDLE wait[2] = {start.ready, g->window_thread};
    WaitForMultipleObjects(2, wait, FALSE, INFINITE);
    CloseHandle(start.ready);
    if (start.what || !g->window) {
        WaitForSingleObject(g->window_thread, INFINITE);
        CloseHandle(g->window_thread); g->window_thread = NULL; g->window = NULL;
        return fail(g, "%s failed: %lu", start.what ? start.what : "window thread", start.error);
    }
    VkWin32SurfaceCreateInfoKHR surface = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = instance, .hwnd = g->window};
    VK_TRY(g, g->vkCreateWin32SurfaceKHR(g->instance, &surface, NULL, &g->surface));
    return 0;
}

static int create_device(DkrGfx *g) {
    uint32_t count = 0;
    VK_TRY(g, g->vkEnumeratePhysicalDevices(g->instance, &count, NULL));
    if (!count) return fail(g, "no Vulkan physical device");
    VkPhysicalDevice *devices = calloc(count, sizeof *devices);
    if (!devices) return fail(g, "out of host memory enumerating GPUs");
    VkResult result = g->vkEnumeratePhysicalDevices(g->instance, &count, devices);
    int best = -1;
    for (uint32_t i = 0; result == VK_SUCCESS && i < count; i++) {
        uint32_t n = 0;
        g->vkEnumerateDeviceExtensionProperties(devices[i], NULL, &n, NULL);
        VkExtensionProperties *extensions = calloc(n ? n : 1, sizeof *extensions);
        if (!extensions) continue;
        VkResult er = g->vkEnumerateDeviceExtensionProperties(devices[i], NULL, &n, extensions);
        int swapchain = 0;
        for (uint32_t k = 0; er == VK_SUCCESS && k < n; k++)
            if (!strcmp(extensions[k].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) swapchain = 1;
        free(extensions);
        if (!swapchain) continue;
        g->vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &n, NULL);
        VkQueueFamilyProperties *families = calloc(n ? n : 1, sizeof *families);
        if (!families) continue;
        g->vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &n, families);
        for (uint32_t k = 0; k < n; k++) {
            VkBool32 present = VK_FALSE;
            er = g->vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], k, g->surface, &present);
            if (er != VK_SUCCESS || !present || !families[k].queueCount || !(families[k].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkPhysicalDeviceProperties properties;
            g->vkGetPhysicalDeviceProperties(devices[i], &properties);
            if (properties.apiVersion < VK_API_VERSION_1_3) continue;
            int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best) { best = score; g->physical = devices[i]; g->family = k; }
        }
        free(families);
    }
    free(devices);
    if (!g->physical) return fail(g, "no GPU with graphics, presentation and VK_KHR_swapchain support");
    VkPhysicalDeviceProperties properties;
    g->vkGetPhysicalDeviceProperties(g->physical, &properties);
    fprintf(stderr, "vulkan: device '%s', API %u.%u.%u, queue family %u\n", properties.deviceName,
        VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
        VK_API_VERSION_PATCH(properties.apiVersion), g->family);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g->family, .queueCount = 1, .pQueuePriorities = &priority};
    const char *extensions[4] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    uint32_t extension_count = 1, available_count = 0;
    VK_TRY(g, g->vkEnumerateDeviceExtensionProperties(g->physical, NULL, &available_count, NULL));
    VkExtensionProperties *available = calloc(available_count ? available_count : 1, sizeof *available);
    if (!available) return fail(g, "out of memory reading draw capabilities");
    result = g->vkEnumerateDeviceExtensionProperties(g->physical, NULL, &available_count, available);
    int has_clip = 0, has_robustness = 0, has_xfb = 0;
    for (unsigned i = 0; result == VK_SUCCESS && i < available_count; i++) {
        if (!strcmp(available[i].extensionName, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME)) has_clip = 1;
        if (!strcmp(available[i].extensionName, VK_EXT_VERTEX_ATTRIBUTE_ROBUSTNESS_EXTENSION_NAME)) has_robustness = 1;
        if (!strcmp(available[i].extensionName, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) has_xfb = 1;
    }
    free(available);
    if (result != VK_SUCCESS) return fail(g, "cannot read draw capabilities: %d", result);
    if (has_clip) extensions[extension_count++] = VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME;
    if (has_robustness) extensions[extension_count++] = VK_EXT_VERTEX_ATTRIBUTE_ROBUSTNESS_EXTENSION_NAME;
    if (has_xfb) extensions[extension_count++] = VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT feedback = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceVertexAttributeRobustnessFeaturesEXT robust = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_ROBUSTNESS_FEATURES_EXT,
        .pNext = has_xfb ? &feedback : NULL
    };
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT,
        .pNext = has_robustness ? (void *)&robust : (has_xfb ? (void *)&feedback : NULL)
    };
    VkPhysicalDeviceVulkan11Features core11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = has_clip ? (void *)&clip : (has_robustness ? (void *)&robust : (has_xfb ? (void *)&feedback : NULL))
    };
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &core11};
    g->vkGetPhysicalDeviceFeatures2(g->physical, &features);
    g->depth_clip_control = has_clip && clip.depthClipControl;
    g->vertex_attribute_robustness = has_robustness && robust.vertexAttributeRobustness;
    g->transform_feedback = has_xfb && feedback.transformFeedback;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT enable_feedback = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, .transformFeedback = g->transform_feedback};
    VkPhysicalDeviceVertexAttributeRobustnessFeaturesEXT enable_robust = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_ROBUSTNESS_FEATURES_EXT,
        .pNext = g->transform_feedback ? &enable_feedback : NULL,
        .vertexAttributeRobustness = g->vertex_attribute_robustness
    };
    VkPhysicalDeviceDepthClipControlFeaturesEXT enable_clip = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT,
        .pNext = g->vertex_attribute_robustness ? (void *)&enable_robust : (g->transform_feedback ? (void *)&enable_feedback : NULL),
        .depthClipControl = g->depth_clip_control
    };
    VkPhysicalDeviceVulkan11Features enable11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = g->depth_clip_control ? (void *)&enable_clip : (g->vertex_attribute_robustness ? (void *)&enable_robust : (g->transform_feedback ? (void *)&enable_feedback : NULL)),
        .shaderDrawParameters = core11.shaderDrawParameters};
    if (!enable11.shaderDrawParameters) return fail(g, "NVN shaders require shaderDrawParameters");
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue, .enabledExtensionCount = extension_count, .ppEnabledExtensionNames = extensions,
        .pEnabledFeatures = &features.features, .pNext = &enable11};
    VK_TRY(g, g->vkCreateDevice(g->physical, &device, NULL, &g->device));
#define LOAD_DEVICE(name) do { \
    g->vk##name = (PFN_vk##name)g->vkGetDeviceProcAddr(g->device, "vk" #name); \
    if (!g->vk##name) return fail(g, "missing Vulkan device entry point vk%s", #name); \
} while (0);
    DEVICE_PROCS(LOAD_DEVICE)
    if (g->transform_feedback) {
        LOAD_DEVICE(CmdBindTransformFeedbackBuffersEXT)
        LOAD_DEVICE(CmdBeginTransformFeedbackEXT)
        LOAD_DEVICE(CmdEndTransformFeedbackEXT)
    }
#undef LOAD_DEVICE
    g->vkGetDeviceQueue(g->device, g->family, 0, &g->queue);
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g->family};
    VK_TRY(g, g->vkCreateCommandPool(g->device, &pool, NULL, &g->command_pool));
    VkCommandBufferAllocateInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g->command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VK_TRY(g, g->vkAllocateCommandBuffers(g->device, &command, &g->command));
    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_TRY(g, g->vkCreateSemaphore(g->device, &semaphore, NULL, &g->acquired));
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VK_TRY(g, g->vkCreateFence(g->device, &fence, NULL, &g->submitted));
    return 0;
}

static void destroy_swapchain(DkrGfx *g) {
    if (g->present_semaphores) {
        for (uint32_t i = 0; i < g->image_count; i++)
            if (g->present_semaphores[i]) g->vkDestroySemaphore(g->device, g->present_semaphores[i], NULL);
    }
    if (g->readback) g->vkDestroyBuffer(g->device, g->readback, NULL);
    if (g->readback_memory) g->vkFreeMemory(g->device, g->readback_memory, NULL);
    if (g->swapchain) g->vkDestroySwapchainKHR(g->device, g->swapchain, NULL);
    free(g->images); free(g->initialized); free(g->present_semaphores);
    g->images = NULL; g->initialized = NULL; g->present_semaphores = NULL;
    g->swapchain = VK_NULL_HANDLE; g->readback = VK_NULL_HANDLE; g->readback_memory = VK_NULL_HANDLE;
    g->image_count = 0;
}

static int create_readback(DkrGfx *g) {
    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = (VkDeviceSize)g->extent.width * g->extent.height * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK_TRY(g, g->vkCreateBuffer(g->device, &buffer, NULL, &g->readback));
    VkMemoryRequirements required;
    VkPhysicalDeviceMemoryProperties memory;
    g->vkGetBufferMemoryRequirements(g->device, g->readback, &required);
    g->vkGetPhysicalDeviceMemoryProperties(g->physical, &memory);
    uint32_t index = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
        if (!(required.memoryTypeBits & (1u << i)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        if (index == UINT32_MAX || (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            index = i;
            g->readback_coherent = !!(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (g->readback_coherent) break;
        }
    }
    if (index == UINT32_MAX) return fail(g, "no host-visible Vulkan readback memory");
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = required.size, .memoryTypeIndex = index};
    VK_TRY(g, g->vkAllocateMemory(g->device, &allocation, NULL, &g->readback_memory));
    VK_TRY(g, g->vkBindBufferMemory(g->device, g->readback, g->readback_memory, 0));
    return 0;
}

static uint32_t clamp_u32(uint32_t value, uint32_t lo, uint32_t hi) { return value < lo ? lo : value > hi ? hi : value; }

/* Returns 1 while minimized; callers keep pumping messages without submitting to a zero-sized surface. */
static int create_swapchain(DkrGfx *g) {
    RECT client;
    if (!GetClientRect(g->window, &client)) return fail(g, "GetClientRect failed");
    if (!client.right || !client.bottom) return 1;
    VkSurfaceCapabilitiesKHR caps;
    VK_TRY(g, g->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g->physical, g->surface, &caps));
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = clamp_u32((uint32_t)client.right, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = clamp_u32((uint32_t)client.bottom, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) return 1;
    uint32_t count = 0;
    VK_TRY(g, g->vkGetPhysicalDeviceSurfaceFormatsKHR(g->physical, g->surface, &count, NULL));
    VkSurfaceFormatKHR *formats = calloc(count ? count : 1, sizeof *formats);
    if (!formats) return fail(g, "out of host memory reading surface formats");
    VkResult result = g->vkGetPhysicalDeviceSurfaceFormatsKHR(g->physical, g->surface, &count, formats);
    VkSurfaceFormatKHR chosen = {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (uint32_t i = 0; result == VK_SUCCESS && i < count; i++) {
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = formats[i]; break; }
        if (count == 1 && formats[i].format == VK_FORMAT_UNDEFINED) chosen.format = VK_FORMAT_B8G8R8A8_UNORM;
    }
    free(formats);
    if (result != VK_SUCCESS || chosen.format == VK_FORMAT_UNDEFINED) return fail(g, "no supported RGBA/BGRA UNORM swapchain format");
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((caps.supportedUsageFlags & usage) != usage) return fail(g, "swapchain images do not support color/transfer/readback usage");
    uint32_t images = caps.minImageCount + 1;
    if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        alpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & (0u - caps.supportedCompositeAlpha));
    if (g->swapchain) {
        VK_TRY(g, g->vkDeviceWaitIdle(g->device));
        destroy_swapchain(g);
    }
    /* vsync setting: FIFO is always available; mailbox and immediate are used when the surface offers them. */
    uint32_t mode_count = 0;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR wanted = g->vsync == 1 ? VK_PRESENT_MODE_MAILBOX_KHR : g->vsync == 2 ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    if (wanted != VK_PRESENT_MODE_FIFO_KHR &&
        g->vkGetPhysicalDeviceSurfacePresentModesKHR(g->physical, g->surface, &mode_count, NULL) == VK_SUCCESS && mode_count) {
        VkPresentModeKHR *modes = calloc(mode_count, sizeof *modes);
        if (modes && g->vkGetPhysicalDeviceSurfacePresentModesKHR(g->physical, g->surface, &mode_count, modes) == VK_SUCCESS)
            for (uint32_t i = 0; i < mode_count; i++) if (modes[i] == wanted) present_mode = wanted;
        free(modes);
    }
    VkSwapchainCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g->surface,
        .minImageCount = images, .imageFormat = chosen.format, .imageColorSpace = chosen.colorSpace,
        .imageExtent = extent, .imageArrayLayers = 1, .imageUsage = usage, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform, .compositeAlpha = alpha, .presentMode = present_mode, .clipped = VK_TRUE};
    VK_TRY(g, g->vkCreateSwapchainKHR(g->device, &info, NULL, &g->swapchain));
    g->format = chosen.format; g->extent = extent;
    VK_TRY(g, g->vkGetSwapchainImagesKHR(g->device, g->swapchain, &g->image_count, NULL));
    g->images = calloc(g->image_count, sizeof *g->images);
    g->initialized = calloc(g->image_count, sizeof *g->initialized);
    g->present_semaphores = calloc(g->image_count, sizeof *g->present_semaphores);
    if (!g->images || !g->initialized || !g->present_semaphores) return fail(g, "out of host memory creating swapchain state");
    VK_TRY(g, g->vkGetSwapchainImagesKHR(g->device, g->swapchain, &g->image_count, g->images));
    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < g->image_count; i++)
        VK_TRY(g, g->vkCreateSemaphore(g->device, &semaphore, NULL, &g->present_semaphores[i]));
    if (create_readback(g)) return -1;
    g->dirty = 0;
    fprintf(stderr, "vulkan: swapchain %ux%u, %u images, format %d, mode %s\n", extent.width, extent.height, g->image_count, g->format,
            present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo");
    return 0;
}

int dkr_gfx_create(DkrGfx **out, const DkrGfxConfig *config) {
    if (!out) return -1;
    DkrGfx *g = calloc(1, sizeof *g);
    *out = g;
    if (!g) return -1;
    if (!config || !config->width || !config->height || config->width > 16384 || config->height > 16384)
        return fail(g, "invalid initial graphics dimensions");
    g->borderless = 0; g->vsync = config->vsync; g->linear = config->linear; g->keep_aspect = config->keep_aspect;
    if (create_instance(g, config->validation) || create_window(g, config) || create_device(g)) return -1;
    return create_swapchain(g) < 0 ? -1 : 0;
}

int dkr_gfx_poll(DkrGfx *g) { /* messages are pumped by the window thread */
    return g && g->window && !g->closed;
}

int dkr_gfx_resize(DkrGfx *g, uint32_t width, uint32_t height) {
    if (!g || !g->window || !width || !height || width > 16384 || height > 16384) return -1;
    RECT rect = {0, 0, (LONG)width, (LONG)height};
    if (!AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE) ||
        !SetWindowPos(g->window, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER))
        return fail(g, "cannot resize the graphics window");
    g->dirty = 1;
    return 0;
}

static void image_barrier(DkrGfx *g, VkImage image, VkImageLayout before, VkImageLayout after,
        VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags from, VkPipelineStageFlags to) {
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src, .dstAccessMask = dst, .oldLayout = before, .newLayout = after,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    g->vkCmdPipelineBarrier(g->command, from, to, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static int write_capture(DkrGfx *g, const char *path) {
    void *data;
    VK_TRY(g, g->vkMapMemory(g->device, g->readback_memory, 0, VK_WHOLE_SIZE, 0, &data));
    if (!g->readback_coherent) {
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = g->readback_memory, .size = VK_WHOLE_SIZE};
        VkResult result = g->vkInvalidateMappedMemoryRanges(g->device, 1, &range);
        if (result != VK_SUCCESS) {
            g->vkUnmapMemory(g->device, g->readback_memory);
            return fail(g, "Vulkan readback invalidation failed: %d", result);
        }
    }
    wchar_t filename[4096];
    FILE *file = NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, filename, 4096)) file = _wfopen(filename, L"wb");
    int ok = file != NULL;
    uint32_t bytes = g->extent.width * g->extent.height * 4;
    BITMAPFILEHEADER header = {.bfType = 0x4d42, .bfSize = sizeof header + sizeof(BITMAPINFOHEADER) + bytes,
        .bfOffBits = sizeof header + sizeof(BITMAPINFOHEADER)};
    BITMAPINFOHEADER bitmap = {.biSize = sizeof bitmap, .biWidth = (LONG)g->extent.width,
        .biHeight = -(LONG)g->extent.height, .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB, .biSizeImage = bytes};
    if (ok) ok = fwrite(&header, 1, sizeof header, file) == sizeof header && fwrite(&bitmap, 1, sizeof bitmap, file) == sizeof bitmap;
    if (ok && g->format == VK_FORMAT_B8G8R8A8_UNORM) ok = fwrite(data, 1, bytes, file) == bytes;
    else if (ok) {
        uint8_t *row = malloc((size_t)g->extent.width * 4);
        if (!row) ok = 0;
        for (uint32_t y = 0; ok && y < g->extent.height; y++) {
            const uint8_t *source = (const uint8_t *)data + (size_t)y * g->extent.width * 4;
            for (uint32_t x = 0; x < g->extent.width; x++) {
                row[4*x] = source[4*x+2]; row[4*x+1] = source[4*x+1];
                row[4*x+2] = source[4*x]; row[4*x+3] = source[4*x+3];
            }
            ok = fwrite(row, 4, g->extent.width, file) == g->extent.width;
        }
        free(row);
    }
    if (file && fclose(file)) ok = 0;
    g->vkUnmapMemory(g->device, g->readback_memory);
    return ok ? 0 : fail(g, "cannot write Vulkan capture '%s'", path);
}

static int present_image(DkrGfx *g, const float color[4], DkrVkImage *source, const char *capture_bmp) {
    if (!g || (!color && !source) || !g->device || g->closed) return -1;
    if (g->batch_active) return fail(g, "presentation during an unfinished draw batch");
    if (source && (source->owner != g || source->info.aspect != VK_IMAGE_ASPECT_COLOR_BIT))
        return fail(g, "presentation requires a color image from this context");
    if (g->dirty) {
        int created = create_swapchain(g);
        if (created < 0) return -1;
        if (created > 0) return 0;
    }
    if (!g->swapchain) return 0;
    VK_TRY(g, g->vkWaitForFences(g->device, 1, &g->submitted, VK_TRUE, 5000000000ull));
    uint32_t index;
    VkResult result = g->vkAcquireNextImageKHR(g->device, g->swapchain, 1000000000ull, g->acquired, VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) { g->dirty = 1; return 0; }
    if (result == VK_TIMEOUT || result == VK_NOT_READY) return 0;
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return fail(g, "vkAcquireNextImageKHR returned %d", result);
    if (result == VK_SUBOPTIMAL_KHR) g->dirty = 1;
    VK_TRY(g, g->vkResetCommandBuffer(g->command, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_TRY(g, g->vkBeginCommandBuffer(g->command, &begin));
    VkImage image = g->images[index];
    /* Match the acquire semaphore's wait stage in the barrier's source scope too: the layout transition must
     * follow acquisition. TOP_OF_PIPE here would allow it to race with the presentation engine. */
    image_barrier(g, image, g->initialized[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (source) {
        if (dkr_vk_image_ready(g, source)) return -1;
        /* Fit the image into the window: letterboxed to its own aspect ratio (bars cleared to black), or stretched. */
        int32_t x0 = 0, y0 = 0, x1 = (int32_t)g->extent.width, y1 = (int32_t)g->extent.height;
        if (g->keep_aspect && source->info.width && source->info.height) {
            uint64_t sw = source->info.width, sh = source->info.height, dw = g->extent.width, dh = g->extent.height;
            if (dw * sh > dh * sw) { int32_t w = (int32_t)(dh * sw / sh); x0 = (int32_t)(dw - (uint64_t)w) / 2; x1 = x0 + w; }
            else if (dw * sh < dh * sw) { int32_t h = (int32_t)(dw * sh / sw); y0 = (int32_t)(dh - (uint64_t)h) / 2; y1 = y0 + h; }
            if (x0 || y0) {
                VkClearColorValue black = {{0, 0, 0, 1}};
                VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                g->vkCmdClearColorImage(g->command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
                image_barrier(g, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            }
        }
        VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{0, 0, 0}, {(int32_t)source->info.width, (int32_t)source->info.height, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets = {{x0, g->present_flip ? y1 : y0, 0}, {x1, g->present_flip ? y0 : y1, 1}}};
        g->vkCmdBlitImage(g->command, source->image, VK_IMAGE_LAYOUT_GENERAL, image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, g->linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    } else {
        VkClearColorValue clear;
        memcpy(clear.float32, color, sizeof clear.float32);
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        g->vkCmdClearColorImage(g->command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }
    if (capture_bmp) {
        image_barrier(g, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {g->extent.width, g->extent.height, 1}};
        g->vkCmdCopyImageToBuffer(g->command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g->readback, 1, &copy);
        VkBufferMemoryBarrier buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = g->readback, .size = VK_WHOLE_SIZE};
        g->vkCmdPipelineBarrier(g->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 0, NULL, 1, &buffer, 0, NULL);
    }
    image_barrier(g, image, capture_bmp ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, capture_bmp ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT,
        0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    VK_TRY(g, g->vkEndCommandBuffer(g->command));
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    /* Presentation completion is tied to the acquired image, not to the CPU frame fence. Keep its semaphore
     * per swapchain image (Khronos Vulkan Guide, "Swapchain Semaphore Reuse"). */
    VkSemaphore finished = g->present_semaphores[index];
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
        .pWaitSemaphores = &g->acquired, .pWaitDstStageMask = &wait, .commandBufferCount = 1,
        .pCommandBuffers = &g->command, .signalSemaphoreCount = 1, .pSignalSemaphores = &finished};
    VK_TRY(g, g->vkResetFences(g->device, 1, &g->submitted));
    VK_TRY(g, g->vkQueueSubmit(g->queue, 1, &submit, g->submitted));
    g->initialized[index] = 1;
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
        .pWaitSemaphores = &finished, .swapchainCount = 1, .pSwapchains = &g->swapchain, .pImageIndices = &index};
    result = g->vkQueuePresentKHR(g->queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) g->dirty = 1;
    else if (result != VK_SUCCESS) return fail(g, "vkQueuePresentKHR returned %d", result);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) g->frames++;
    if (capture_bmp) {
        VK_TRY(g, g->vkWaitForFences(g->device, 1, &g->submitted, VK_TRUE, 5000000000ull));
        if (write_capture(g, capture_bmp)) return -1;
    }
    if (g->validation_errors) return fail(g, "Vulkan reported validation errors");
    return 0;
}

int dkr_gfx_clear_present(DkrGfx *g, const float color[4], const char *capture_bmp) {
    return present_image(g, color, NULL, capture_bmp);
}

int dkr_vk_image_present(DkrGfx *g, DkrVkImage *source, const char *capture_bmp) {
    return present_image(g, NULL, source, capture_bmp);
}

const char *dkr_gfx_error(const DkrGfx *g) { return g ? g->error : "out of host memory"; }
void dkr_gfx_set_title(DkrGfx *g, const char *utf8) {
    if (!g || !g->window_thread || !utf8) return;
    wchar_t *text = calloc(256, sizeof *text);
    if (!text) return;
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, text, 256) ||
        !PostThreadMessageW(GetThreadId(g->window_thread), WM_DKR_TITLE, 0, (LPARAM)text)) free(text);
}
uint64_t dkr_gfx_frames(const DkrGfx *g) { return g ? g->frames : 0; }
void dkr_gfx_set_present_flip(DkrGfx *g, int flip) { if (g) g->present_flip = flip != 0; }
uint32_t dkr_gfx_validation_errors(const DkrGfx *g) { return g ? (uint32_t)g->validation_errors : 0; }

uint32_t dkr_gfx_destroy(DkrGfx *g) {
    if (!g) return 0;
    if (g->device) {
        if (g->vkDeviceWaitIdle) g->vkDeviceWaitIdle(g->device);
        dkr_vk_resources_destroy(g);
        destroy_swapchain(g);
        if (g->command_pool) g->vkDestroyCommandPool(g->device, g->command_pool, NULL);
        if (g->acquired) g->vkDestroySemaphore(g->device, g->acquired, NULL);
        if (g->submitted) g->vkDestroyFence(g->device, g->submitted, NULL);
        if (g->vkDestroyDevice) g->vkDestroyDevice(g->device, NULL);
    }
    if (g->surface) g->vkDestroySurfaceKHR(g->instance, g->surface, NULL);
    if (g->debug && g->destroy_debug) g->destroy_debug(g->instance, g->debug, NULL);
    if (g->instance && g->vkDestroyInstance) g->vkDestroyInstance(g->instance, NULL);
    if (g->window_thread) {
        PostThreadMessageW(GetThreadId(g->window_thread), WM_DKR_DESTROY, 0, 0);
        WaitForSingleObject(g->window_thread, 5000);
        CloseHandle(g->window_thread);
    }
    if (g->loader) FreeLibrary(g->loader);
    uint32_t errors = (uint32_t)g->validation_errors;
    free(g);
    return errors;
}
