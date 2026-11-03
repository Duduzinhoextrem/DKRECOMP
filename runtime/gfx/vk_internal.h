#ifndef DKR_VK_INTERNAL_H
#define DKR_VK_INTERNAL_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include "vk_draw.h"

#define INSTANCE_PROCS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceFormatProperties) \
    X(EnumerateDeviceExtensionProperties) X(GetPhysicalDeviceSurfaceSupportKHR) \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR) X(GetPhysicalDeviceSurfaceFormatsKHR) \
    X(GetPhysicalDeviceSurfacePresentModesKHR) \
    X(CreateDevice) X(GetDeviceProcAddr) X(CreateWin32SurfaceKHR) X(DestroySurfaceKHR)
#define DEVICE_PROCS(X) \
    X(DestroyDevice) X(DeviceWaitIdle) X(GetDeviceQueue) X(CreateSwapchainKHR) X(DestroySwapchainKHR) \
    X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR) X(CreateCommandPool) \
    X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) \
    X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdClearColorImage) X(CmdClearDepthStencilImage) X(CmdCopyImageToBuffer) \
    X(QueueSubmit) X(CreateSemaphore) X(DestroySemaphore) X(CreateFence) X(DestroyFence) \
    X(WaitForFences) X(ResetFences) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
    X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(InvalidateMappedMemoryRanges) \
    X(FlushMappedMemoryRanges) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
    X(CreateImageView) X(DestroyImageView) X(CreateShaderModule) X(DestroyShaderModule) X(CreateSampler) X(DestroySampler) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(ResetDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
    X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
    X(CreateGraphicsPipelines) X(DestroyPipeline) X(CmdBeginRenderPass) X(CmdEndRenderPass) \
    X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) \
    X(CmdDraw) X(CmdDrawIndexed) X(CmdSetViewport) X(CmdSetScissor) X(CmdSetBlendConstants) \
    X(CmdSetStencilReference) X(CmdSetStencilCompareMask) X(CmdSetStencilWriteMask) X(CmdSetDepthBias) \
    X(CmdCopyBufferToImage) X(CmdCopyImage) X(CmdBlitImage) X(CmdClearAttachments)

typedef struct DkrVkResources DkrVkResources;
struct DkrGfx {
    HMODULE loader;
    HWND window;
    HANDLE window_thread;          /* owns the window and pumps its messages (see create_window) */
    volatile LONG closed, dirty;   /* written by the window thread, read by the presenting thread */
    int validation, present_flip;
    int borderless, vsync, linear, keep_aspect;  /* from DkrGfxConfig */
    RECT windowed;                               /* window rectangle to restore when leaving borderless */
    volatile LONG validation_errors;
    char error[512];
    uint64_t frames;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug;
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage *images;
    uint8_t *initialized;
    VkSemaphore *present_semaphores;
    VkSemaphore acquired;
    VkFence submitted;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    int readback_coherent, depth_clip_control, vertex_attribute_robustness, batch_active;
    int transform_feedback;
    PFN_vkCmdBindTransformFeedbackBuffersEXT vkCmdBindTransformFeedbackBuffersEXT;
    PFN_vkCmdBeginTransformFeedbackEXT vkCmdBeginTransformFeedbackEXT;
    PFN_vkCmdEndTransformFeedbackEXT vkCmdEndTransformFeedbackEXT;
    DkrVkResources *resources;
    PFN_vkGetInstanceProcAddr get_instance_proc;
#define DECLARE_PROC(name) PFN_vk##name vk##name;
    INSTANCE_PROCS(DECLARE_PROC)
    DEVICE_PROCS(DECLARE_PROC)
#undef DECLARE_PROC
};

int dkr_vk_fail(DkrGfx *gfx, const char *format, ...);
#define VK_TRY(g, call) do { VkResult result_ = (call); \
    if (result_ != VK_SUCCESS) return dkr_vk_fail(g, "%s returned VkResult %d", #call, (int)result_); } while (0)
#endif
