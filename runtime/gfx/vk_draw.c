/* Draw/resource backend for host NVN replay. All Vulkan objects belong to one DkrGfx instance.
 * Batches currently complete synchronously. This intentionally favors a verifiable lifetime model before
 * introducing asynchronous submissions; there are no guessed "GPU complete" fences in this layer. */
#include "vk_internal.h"
#include "perf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Pipeline {
    struct Pipeline *next;
    DkrVkPipelineInfo info;
    VkPipeline pipeline;
    VkRenderPass pass;
} Pipeline;

struct DkrVkSampler { DkrVkSampler *next; DkrGfx *owner; VkSampler sampler; };
struct DkrVkProgram {
    DkrVkProgram *next;
    DkrGfx *owner;
    VkShaderModule shaders[DKR_VK_STAGES];
    VkDescriptorSetLayout sets[DKR_VK_STAGES];
    VkPipelineLayout layout;
    unsigned binding_count;
    DkrVkBinding bindings[DKR_VK_BINDINGS];
    Pipeline *pipelines;
};
typedef struct Framebuffer {
    struct Framebuffer *next;
    VkRenderPass pass;
    uint32_t view_count, width, height;
    VkImageView views[9];
    VkFramebuffer framebuffer;
} Framebuffer;
typedef struct RenderPass {
    struct RenderPass *next;
    uint32_t color_count;
    VkFormat colors[8], depth;
    VkRenderPass pass;
} RenderPass;
static int render_pass(DkrGfx *g, const DkrVkPipelineInfo *info, VkRenderPass *out);
/* Transient uploads (vertex/index/uniform data, texture staging) are carved out of large persistently mapped
 * chunks instead of one VkBuffer + vkAllocateMemory each. Batches wait for the GPU before the next one begins,
 * so every chunk is rewound in recycle(). Chunks are kept for reuse; a larger upload gets its own chunk. */
typedef struct Chunk {
    struct Chunk *next;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *mapped;
    VkDeviceSize size, used;
    int coherent;
} Chunk;
#define ARENA_CHUNK (64ull << 20)
struct DkrVkResources {
    DkrVkBuffer *buffers;
    DkrVkBuffer *free_temp_buffers;
    DkrVkImage *images;
    DkrVkSampler *samplers;
    DkrVkProgram *programs;
    Framebuffer *framebuffers;
    RenderPass *render_passes;
    VkRenderPass active_pass;
    VkFramebuffer active_framebuffer;
    VkDescriptorPool descriptors;
    VkPhysicalDeviceProperties properties;
    DkrVkImage *snapshot;
    Chunk *chunks;
    VkDeviceSize arena_align;
};

static DkrVkResources *resources(DkrGfx *g) {
    if (!g->resources) {
        g->resources = calloc(1, sizeof *g->resources);
        if (g->resources) g->vkGetPhysicalDeviceProperties(g->physical, &g->resources->properties);
    }
    if (!g->resources) dkr_vk_fail(g, "out of memory creating draw state");
    return g->resources;
}

static uint32_t memory_type(DkrGfx *g, uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags prefer,
                             VkMemoryPropertyFlags *actual) {
    VkPhysicalDeviceMemoryProperties memory;
    g->vkGetPhysicalDeviceMemoryProperties(g->physical, &memory);
    uint32_t selected = UINT32_MAX;
    for (unsigned i = 0; i < memory.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = memory.memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (f & need) != need) continue;
        if (selected == UINT32_MAX || (f & prefer) == prefer) {
            selected = i; *actual = f;
            if ((f & prefer) == prefer) break;
        }
    }
    return selected;
}

int dkr_vk_image_create(DkrGfx *g, const DkrVkImageInfo *info, DkrVkImage **out) {
    if (!g || !out || !info) return -1;
    *out = NULL;
    if (!info->width || !info->height || !info->levels || !info->layers || info->levels > 15 ||
        info->width > 16384 || info->height > 16384 || info->layers > 2048 || !info->aspect)
        return dkr_vk_fail(g, "invalid image dimensions/aspects");
    uint32_t largest = info->width > info->height ? info->width : info->height;
    if ((largest >> (info->levels - 1)) == 0) return dkr_vk_fail(g, "image has more mip levels than its extent");
    VkFormatProperties format;
    g->vkGetPhysicalDeviceFormatProperties(g->physical, info->format, &format);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format.optimalTilingFeatures & need) != need) return dkr_vk_fail(g, "image format %d lacks sampling/transfer support", info->format);
    DkrVkResources *r = resources(g);
    if (!r) return -1;
    DkrVkImage *image = calloc(1, sizeof *image);
    if (!image) return dkr_vk_fail(g, "out of memory creating an image");
    image->owner = g; image->info = *info; image->next = r->images; r->images = image; *out = image;
    image->usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((info->aspect & VK_IMAGE_ASPECT_COLOR_BIT) && (format.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        image->usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((info->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) &&
        (format.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        image->usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) image->usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateFlags image_flags = info->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    if (info->layers >= 6 && info->width == info->height) image_flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkImageCreateInfo create = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = image_flags, .imageType = VK_IMAGE_TYPE_2D,
        .format = info->format, .extent = {info->width, info->height, 1}, .mipLevels = info->levels,
        .arrayLayers = info->layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = image->usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK_TRY(g, g->vkCreateImage(g->device, &create, NULL, &image->image));
    VkMemoryRequirements required;
    g->vkGetImageMemoryRequirements(g->device, image->image, &required);
    VkMemoryPropertyFlags flags = 0;
    uint32_t index = memory_type(g, required.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &flags);
    if (index == UINT32_MAX) return dkr_vk_fail(g, "no memory type for image");
    VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = required.size, .memoryTypeIndex = index};
    VK_TRY(g, g->vkAllocateMemory(g->device, &allocate, NULL, &image->memory));
    VK_TRY(g, g->vkBindImageMemory(g->device, image->image, image->memory, 0));
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image->image,
        .viewType = info->layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = info->format,
        .subresourceRange = {info->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : info->aspect,
                            0, info->levels, 0, info->layers}};
    VK_TRY(g, g->vkCreateImageView(g->device, &view, NULL, &image->view));
    if (image->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.subresourceRange = (VkImageSubresourceRange){info->aspect, 0, 1, 0, 1};
        VK_TRY(g, g->vkCreateImageView(g->device, &view, NULL, &image->target_view));
    }
    return 0;
}

int dkr_vk_image_view(DkrGfx *g, DkrVkImage *image, const DkrVkViewInfo *info, DkrVkView **out) {
    if (!g || !image || image->owner != g || !info || !out) return -1;
    *out = NULL;
    if (!info->levels || !info->layers || info->base_level >= image->info.levels || info->levels > image->info.levels - info->base_level ||
        info->base_layer >= image->info.layers || info->layers > image->info.layers - info->base_layer ||
        !info->aspects || (info->aspects & ~image->info.aspect)) return dkr_vk_fail(g, "image view exceeds its storage");
    for (DkrVkView *v = image->views; v; v = v->next) if (!memcmp(&v->info, info, sizeof *info)) { *out = v; return 0; }
    VkFormatProperties format;
    g->vkGetPhysicalDeviceFormatProperties(g->physical, info->format, &format);
    VkImageUsageFlags usage = image->usage;
    if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
    if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) usage &= ~VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) usage &= ~VK_IMAGE_USAGE_SAMPLED_BIT;
    DkrVkView *v = calloc(1, sizeof *v);
    if (!v) return dkr_vk_fail(g, "out of memory creating image view");
    v->image = image; v->info = *info; v->next = image->views; image->views = v; *out = v;
    VkImageViewUsageCreateInfo limited = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = usage};
    VkImageViewCreateInfo create = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &limited,
        .image = image->image, .viewType = info->type, .format = info->format, .components = info->components,
        .subresourceRange = {info->aspects, info->base_level, info->levels, info->base_layer, info->layers}};
    VK_TRY(g, g->vkCreateImageView(g->device, &create, NULL, &v->view));
    return 0;
}

int dkr_vk_buffer_create(DkrGfx *g, const void *data, size_t size, DkrVkBuffer **out) {
    if (!g || !out) return -1;
    *out = NULL;
    if (!size || size > (1u << 30)) return dkr_vk_fail(g, "invalid buffer size %zu", size);
    DkrVkResources *r = resources(g);
    if (!r) return -1;
    DkrVkBuffer *b = calloc(1, sizeof *b);
    if (!b) return dkr_vk_fail(g, "out of memory creating a buffer");
    b->owner = g; b->size = size; b->next = r->buffers; r->buffers = b; *out = b;
    VkBufferCreateInfo create = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (g->transform_feedback) create.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    VK_TRY(g, g->vkCreateBuffer(g->device, &create, NULL, &b->buffer));
    VkMemoryRequirements required;
    g->vkGetBufferMemoryRequirements(g->device, b->buffer, &required);
    VkMemoryPropertyFlags flags = 0;
    uint32_t index = memory_type(g, required.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &flags);
    if (index == UINT32_MAX) return dkr_vk_fail(g, "no host-visible buffer memory");
    VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = required.size, .memoryTypeIndex = index};
    VK_TRY(g, g->vkAllocateMemory(g->device, &allocate, NULL, &b->memory));
    VK_TRY(g, g->vkBindBufferMemory(g->device, b->buffer, b->memory, 0));
    VK_TRY(g, g->vkMapMemory(g->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped));
    b->coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (data) memcpy(b->mapped, data, size); else memset(b->mapped, 0, size);
    if (!b->coherent) {
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = b->memory, .size = VK_WHOLE_SIZE};
        VkResult result = g->vkFlushMappedMemoryRanges(g->device, 1, &range);
        if (result != VK_SUCCESS) return dkr_vk_fail(g, "buffer flush failed: %d", result);
    }
    return 0;
}

int dkr_vk_feedback_supported(DkrGfx *g) { return g && g->transform_feedback; }

int dkr_vk_buffer_read(DkrGfx *g, DkrVkBuffer *buffer, size_t offset, void *out, size_t size) {
    if (!g || g->batch_active || !buffer || buffer->owner != g || buffer->temporary || !out ||
        offset > buffer->size || size > buffer->size - offset) return -1;
    if (!buffer->coherent) {
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = buffer->memory, .size = VK_WHOLE_SIZE};
        VkResult result = g->vkInvalidateMappedMemoryRanges(g->device, 1, &range);
        if (result != VK_SUCCESS) return dkr_vk_fail(g, "feedback readback failed: %d", result);
    }
    memcpy(out, (uint8_t *)buffer->mapped + offset, size);
    return 0;
}

static Chunk *chunk_create(DkrGfx *g, VkDeviceSize size) {
    Chunk *k = calloc(1, sizeof *k);
    if (!k) { dkr_vk_fail(g, "out of memory creating an upload chunk"); return NULL; }
    k->size = size;
    VkBufferCreateInfo create = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkMemoryRequirements required;
    VkMemoryPropertyFlags flags = 0;
    uint32_t index;
    if (g->vkCreateBuffer(g->device, &create, NULL, &k->buffer) != VK_SUCCESS) goto fail;
    g->vkGetBufferMemoryRequirements(g->device, k->buffer, &required);
    index = memory_type(g, required.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &flags);
    if (index == UINT32_MAX) goto fail;
    VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = required.size, .memoryTypeIndex = index};
    if (g->vkAllocateMemory(g->device, &allocate, NULL, &k->memory) != VK_SUCCESS ||
        g->vkBindBufferMemory(g->device, k->buffer, k->memory, 0) != VK_SUCCESS ||
        g->vkMapMemory(g->device, k->memory, 0, VK_WHOLE_SIZE, 0, (void **)&k->mapped) != VK_SUCCESS) goto fail;
    k->coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return k;
fail:
    if (k->buffer) g->vkDestroyBuffer(g->device, k->buffer, NULL);
    if (k->memory) g->vkFreeMemory(g->device, k->memory, NULL);
    free(k);
    dkr_vk_fail(g, "cannot create a %llu-byte upload chunk", (unsigned long long)size);
    return NULL;
}

static void chunk_destroy(DkrGfx *g, Chunk *k) {
    if (k->mapped) g->vkUnmapMemory(g->device, k->memory);
    if (k->buffer) g->vkDestroyBuffer(g->device, k->buffer, NULL);
    if (k->memory) g->vkFreeMemory(g->device, k->memory, NULL);
    free(k);
}

int dkr_vk_buffer_temporary(DkrGfx *g, const void *data, size_t size, DkrVkBuffer **out) {
    if (!g || !g->batch_active) return g ? dkr_vk_fail(g, "temporary buffer outside a batch") : -1;
    if (!out) return -1;
    *out = NULL;
    if (!size || size > (1u << 30)) return dkr_vk_fail(g, "invalid buffer size %zu", size);
    DkrVkResources *r = resources(g);
    if (!r) return -1;
    if (!r->arena_align) {
        /* One alignment that satisfies uniform/storage descriptors, index buffers and copy offsets of any format. */
        VkDeviceSize a = 256;
        const VkPhysicalDeviceLimits *l = &r->properties.limits;
        if (l->minUniformBufferOffsetAlignment > a) a = l->minUniformBufferOffsetAlignment;
        if (l->minStorageBufferOffsetAlignment > a) a = l->minStorageBufferOffsetAlignment;
        r->arena_align = a;
    }
    Chunk *k = r->chunks;
    VkDeviceSize offset = 0;
    for (; k; k = k->next) {
        offset = (k->used + r->arena_align - 1) & ~(r->arena_align - 1);
        if (offset <= k->size && size <= k->size - offset) break;
    }
    if (!k) {
        VkDeviceSize want = size > ARENA_CHUNK ? (size + 0xffff) & ~0xffffull : ARENA_CHUNK;
        if (!(k = chunk_create(g, want))) return -1;
        k->next = r->chunks; r->chunks = k; offset = 0;
    }
    DkrVkBuffer *b = r->free_temp_buffers;
    if (b) { r->free_temp_buffers = b->next; memset(b, 0, sizeof *b); }
    else b = calloc(1, sizeof *b);
    if (!b) return dkr_vk_fail(g, "out of memory creating a temporary buffer");
    b->owner = g; b->buffer = k->buffer; b->size = size; b->base = offset; b->temporary = 1;
    b->next = r->buffers; r->buffers = b; *out = b;
    if (data) memcpy(k->mapped + offset, data, size); else memset(k->mapped + offset, 0, size);
    k->used = offset + size;
    DKR_PERF_ADD(temp_buffers, 1); DKR_PERF_ADD(temp_bytes, (int64_t)size);
    return 0;
}

static void destroy_buffer(DkrGfx *g, DkrVkBuffer *b) {
    if (b->temporary) { free(b); return; } /* an arena slice owns no Vulkan object */
    if (b->mapped) g->vkUnmapMemory(g->device, b->memory);
    if (b->buffer) g->vkDestroyBuffer(g->device, b->buffer, NULL);
    if (b->memory) g->vkFreeMemory(g->device, b->memory, NULL);
    free(b);
}

static void destroy_framebuffers(DkrGfx *g) {
    DkrVkResources *r = g->resources;
    if (!r) return;
    while (r->framebuffers) {
        Framebuffer *f = r->framebuffers; r->framebuffers = f->next;
        g->vkDestroyFramebuffer(g->device, f->framebuffer, NULL); free(f);
    }
}

static void recycle(DkrGfx *g) {
    DkrVkResources *r = g->resources;
    if (!r) return;
    for (DkrVkBuffer **link = &r->buffers; *link;) {
        DkrVkBuffer *b = *link;
        if (b->temporary) { *link = b->next; b->next = r->free_temp_buffers; r->free_temp_buffers = b; }
        else link = &b->next;
    }
    for (Chunk *k = r->chunks; k; k = k->next) k->used = 0;
}

void dkr_vk_buffer_destroy(DkrGfx *g, DkrVkBuffer *b) {
    if (!g || !b || b->owner != g) return;
    if (g->batch_active) { dkr_vk_fail(g, "cannot destroy a buffer during a batch"); return; }
    g->vkDeviceWaitIdle(g->device);
    DkrVkBuffer **link = &g->resources->buffers;
    while (*link && *link != b) link = &(*link)->next;
    if (*link) { *link = b->next; destroy_buffer(g, b); }
}

void dkr_vk_image_destroy(DkrGfx *g, DkrVkImage *image) {
    if (!g || !image || image->owner != g) return;
    if (g->batch_active) { dkr_vk_fail(g, "cannot destroy an image during a batch"); return; }
    g->vkDeviceWaitIdle(g->device); recycle(g); destroy_framebuffers(g);
    DkrVkImage **link = &g->resources->images;
    while (*link && *link != image) link = &(*link)->next;
    if (!*link) return;
    *link = image->next;
    while (image->views) { DkrVkView *view = image->views; image->views = view->next;
        if (view->view) g->vkDestroyImageView(g->device, view->view, NULL); free(view); }
    if (image->view) g->vkDestroyImageView(g->device, image->view, NULL);
    if (image->target_view) g->vkDestroyImageView(g->device, image->target_view, NULL);
    if (image->image) g->vkDestroyImage(g->device, image->image, NULL);
    if (image->memory) g->vkFreeMemory(g->device, image->memory, NULL);
    free(image);
}

int dkr_vk_sampler_create(DkrGfx *g, const VkSamplerCreateInfo *info, DkrVkSampler **out) {
    if (!g || !info || !out) return -1;
    DkrVkResources *r = resources(g);
    if (!r) return -1;
    DkrVkSampler *s = calloc(1, sizeof *s);
    if (!s) return dkr_vk_fail(g, "out of memory creating sampler");
    s->owner = g; s->next = r->samplers; r->samplers = s; *out = s;
    VK_TRY(g, g->vkCreateSampler(g->device, info, NULL, &s->sampler));
    return 0;
}

static void end_render_pass(DkrGfx *g) {
    if (g && g->resources && g->resources->active_framebuffer) {
        g->vkCmdEndRenderPass(g->command);
        g->resources->active_framebuffer = VK_NULL_HANDLE;
        g->resources->active_pass = VK_NULL_HANDLE;
    }
}

static int get_framebuffer(DkrGfx *g, VkRenderPass pass, uint32_t view_count, const VkImageView *views,
                           uint32_t width, uint32_t height, VkFramebuffer *out) {
    DkrVkResources *r = g->resources;
    for (Framebuffer *f = r->framebuffers; f; f = f->next) {
        if (f->pass == pass && f->view_count == view_count && f->width == width && f->height == height &&
            !memcmp(f->views, views, view_count * sizeof *views)) {
            *out = f->framebuffer;
            return 0;
        }
    }
    Framebuffer *frame = calloc(1, sizeof *frame);
    if (!frame) return dkr_vk_fail(g, "out of memory creating framebuffer");
    frame->pass = pass; frame->view_count = view_count; frame->width = width; frame->height = height;
    memcpy(frame->views, views, view_count * sizeof *views);
    VkFramebufferCreateInfo create = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = pass,
        .attachmentCount = view_count, .pAttachments = views, .width = width, .height = height, .layers = 1};
    VK_TRY(g, g->vkCreateFramebuffer(g->device, &create, NULL, &frame->framebuffer));
    frame->next = r->framebuffers; r->framebuffers = frame;
    *out = frame->framebuffer;
    return 0;
}

int dkr_vk_batch_resume(DkrGfx *g) {
    if (!g || !g->device) return -1;
    if (g->batch_active) return dkr_vk_fail(g, "nested Vulkan batch");
    VK_TRY(g, g->vkWaitForFences(g->device, 1, &g->submitted, VK_TRUE, 5000000000ull));
    DkrVkResources *r = resources(g);
    if (!r) return -1;
    r->active_pass = VK_NULL_HANDLE;
    r->active_framebuffer = VK_NULL_HANDLE;
    if (r->descriptors) VK_TRY(g, g->vkResetDescriptorPool(g->device, r->descriptors, 0));
    else {
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 16384}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16384}};
        VkDescriptorPoolCreateInfo create = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 4096, .poolSizeCount = sizeof sizes / sizeof *sizes, .pPoolSizes = sizes};
        VK_TRY(g, g->vkCreateDescriptorPool(g->device, &create, NULL, &r->descriptors));
    }
    VK_TRY(g, g->vkResetCommandBuffer(g->command, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_TRY(g, g->vkBeginCommandBuffer(g->command, &begin));
    g->batch_active = 1;
    return 0;
}

int dkr_vk_batch_begin(DkrGfx *g) {
    if (!g || !g->device) return -1;
    if (g->batch_active) return dkr_vk_fail(g, "nested Vulkan batch");
    VK_TRY(g, g->vkWaitForFences(g->device, 1, &g->submitted, VK_TRUE, 5000000000ull));
    recycle(g);
    return dkr_vk_batch_resume(g);
}

int dkr_vk_batch_end(DkrGfx *g) {
    if (!g || !g->batch_active) return g ? dkr_vk_fail(g, "end without a Vulkan batch") : -1;
    end_render_pass(g);
    VK_TRY(g, g->vkEndCommandBuffer(g->command));
    if (g->resources) for (Chunk *k = g->resources->chunks; k; k = k->next) if (k->used && !k->coherent) {
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = k->memory, .size = VK_WHOLE_SIZE};
        VK_TRY(g, g->vkFlushMappedMemoryRanges(g->device, 1, &range));
    }
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g->command};
    VK_TRY(g, g->vkResetFences(g->device, 1, &g->submitted));
    VK_TRY(g, g->vkQueueSubmit(g->queue, 1, &submit, g->submitted));
    g->batch_active = 0;
    int64_t t0 = dkr_perf_on ? dkr_perf_now() : 0;
    VK_TRY(g, g->vkWaitForFences(g->device, 1, &g->submitted, VK_TRUE, 5000000000ull));
    DKR_PERF_ADD(gpu_wait, dkr_perf_now() - t0);
    return g->validation_errors ? dkr_vk_fail(g, "Vulkan draw batch reported validation errors") : 0;
}

int dkr_vk_image_ready(DkrGfx *g, DkrVkImage *image) {
    if (!image || image->owner != g) return dkr_vk_fail(g, "image belongs to a different graphics context");
    end_render_pass(g);
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = image->initialized ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = image->initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image->image,
        .subresourceRange = {image->info.aspect, 0, image->info.levels, 0, image->info.layers}};
    g->vkCmdPipelineBarrier(g->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier);
    image->initialized = 1;
    image->dirty = 0;
    return 0;
}

static int image_ready_read(DkrGfx *g, DkrVkImage *image) {
    if (!image || image->owner != g) return dkr_vk_fail(g, "image belongs to a different graphics context");
    if (!image->initialized || image->dirty) return dkr_vk_image_ready(g, image);
    return 0;
}

int dkr_vk_image_clear(DkrGfx *g, DkrVkImage *image, const float color[4]) {
    if (!g || !g->batch_active || !color) return -1;
    if (!image || image->info.aspect != VK_IMAGE_ASPECT_COLOR_BIT) return dkr_vk_fail(g, "color clear requires a color image");
    if (dkr_vk_image_ready(g, image)) return -1;
    VkClearColorValue value;
    memcpy(value.float32, color, sizeof value.float32);
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image->info.levels, 0, image->info.layers};
    g->vkCmdClearColorImage(g->command, image->image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    image->dirty = 1;
    return 0;
}

int dkr_vk_image_clear_depth(DkrGfx *g, DkrVkImage *image, VkImageAspectFlags aspects, float depth, uint32_t stencil) {
    if (!g || !g->batch_active || !image) return -1;
    if (!aspects || (aspects & ~image->info.aspect) || (aspects & VK_IMAGE_ASPECT_COLOR_BIT))
        return dkr_vk_fail(g, "invalid depth/stencil clear aspects");
    if (dkr_vk_image_ready(g, image)) return -1;
    VkClearDepthStencilValue value = {depth, stencil};
    VkImageSubresourceRange range = {aspects, 0, 1, 0, 1};
    g->vkCmdClearDepthStencilImage(g->command, image->image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    image->dirty = 1;
    return 0;
}

int dkr_vk_attachment_clear(DkrGfx *g, DkrVkImage *image, DkrVkView *view, VkRect2D rect,
                            VkImageAspectFlags aspects, const VkClearValue *value) {
    if (!g || !g->batch_active || !image || !view || !value) return -1;
    int depth = aspects != VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (view->image != image || image->owner != g || !aspects || (aspects & ~image->info.aspect) ||
        (depth && (aspects & VK_IMAGE_ASPECT_COLOR_BIT)) || !(image->usage & usage) ||
        view->info.levels != 1 || view->info.layers != 1)
        return dkr_vk_fail(g, "invalid clear attachment view/aspects");
    unsigned width = image->info.width >> view->info.base_level, height = image->info.height >> view->info.base_level;
    if (!width) width = 1; if (!height) height = 1;
    if (rect.offset.x < 0 || rect.offset.y < 0 || (uint64_t)rect.offset.x + rect.extent.width > width ||
        (uint64_t)rect.offset.y + rect.extent.height > height) return dkr_vk_fail(g, "clear rectangle exceeds attachment");
    if (!rect.extent.width || !rect.extent.height) return 0;
    DkrVkResources *r = resources(g);
    if (!r || dkr_vk_image_ready(g, image)) return -1;
    DkrVkPipelineInfo info = {0};
    if (depth) info.depth = view->info.format;
    else { info.color_count = 1; info.colors[0] = view->info.format; }
    VkRenderPass pass = VK_NULL_HANDLE;
    if (render_pass(g, &info, &pass)) return -1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (get_framebuffer(g, pass, 1, &view->view, width, height, &fb)) return -1;
    VkRenderPassBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass,
        .framebuffer = fb, .renderArea = {{0, 0}, {width, height}}};
    VkClearAttachment clear = {.aspectMask = aspects, .colorAttachment = 0, .clearValue = *value};
    VkClearRect area = {.rect = rect, .baseArrayLayer = 0, .layerCount = 1};
    g->vkCmdBeginRenderPass(g->command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    g->vkCmdClearAttachments(g->command, 1, &clear, 1, &area);
    g->vkCmdEndRenderPass(g->command);
    image->dirty = 1;
    return 0;
}

static size_t level_bytes(VkFormat format, uint32_t width, uint32_t height) {
    unsigned pixel = 0;
    switch (format) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: pixel = 1; break;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_SFLOAT: pixel = 2; break;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R8G8B8A8_UINT: pixel = 4; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: pixel = 8; break;
    case VK_FORMAT_R32G32B32A32_SFLOAT: pixel = 16; break;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK:
        return (size_t)((width + 3) / 4) * ((height + 3) / 4) * 8;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK: return (size_t)((width + 3) / 4) * ((height + 3) / 4) * 16;
    default: return 0;
    }
    return (size_t)width * height * pixel;
}

int dkr_vk_image_upload(DkrGfx *g, DkrVkImage *image, unsigned level, unsigned layer, const void *data, size_t size) {
    if (!image || level >= image->info.levels) return -1;
    unsigned width = image->info.width >> level, height = image->info.height >> level;
    return dkr_vk_image_upload_region(g, image, level, layer, 0, 0, width ? width : 1, height ? height : 1, data, size);
}

int dkr_vk_image_upload_region(DkrGfx *g, DkrVkImage *image, unsigned level, unsigned layer,
                                unsigned x, unsigned y, unsigned width, unsigned height, const void *data, size_t size) {
    if (!g || !g->batch_active || !image || image->owner != g || !data) return -1;
    if (level >= image->info.levels || layer >= image->info.layers || image->info.aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return dkr_vk_fail(g, "invalid image upload subresource");
    unsigned full_width = image->info.width >> level, full_height = image->info.height >> level;
    if (!full_width) full_width = 1; if (!full_height) full_height = 1;
    if (!width || !height || x > full_width || width > full_width - x || y > full_height || height > full_height - y)
        return dkr_vk_fail(g, "image upload rectangle exceeds its mip");
    size_t expected = level_bytes(image->info.format, width, height);
    if (!expected || expected != size) return dkr_vk_fail(g, "unsupported format or wrong image upload size (%zu/%zu)", size, expected);
    DkrVkBuffer *staging;
    if (dkr_vk_buffer_temporary(g, data, size, &staging) || dkr_vk_image_ready(g, image)) return -1;
    VkBufferImageCopy region = {.bufferOffset = staging->base, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1},
        .imageOffset = {(int32_t)x, (int32_t)y, 0}, .imageExtent = {width, height, 1}};
    g->vkCmdCopyBufferToImage(g->command, staging->buffer, image->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    image->dirty = 1;
    return 0;
}

int dkr_vk_image_copy(DkrGfx *g, DkrVkImage *source, unsigned sl, unsigned sa, unsigned sx, unsigned sy,
                      DkrVkImage *destination, unsigned dl, unsigned da, unsigned dx, unsigned dy, unsigned width, unsigned height) {
    if (!g || !g->batch_active || !source || !destination || source->owner != g || destination->owner != g) return -1;
    if (source == destination || source->info.format != destination->info.format || source->info.aspect != destination->info.aspect ||
        sl >= source->info.levels || dl >= destination->info.levels || sa >= source->info.layers || da >= destination->info.layers)
        return dkr_vk_fail(g, "image copy needs matching distinct images/subresources");
    unsigned sw = source->info.width >> sl, sh = source->info.height >> sl, dw = destination->info.width >> dl, dh = destination->info.height >> dl;
    if (!sw) sw = 1; if (!sh) sh = 1; if (!dw) dw = 1; if (!dh) dh = 1;
    if (!width || !height || sx > sw || width > sw-sx || sy > sh || height > sh-sy || dx > dw || width > dw-dx || dy > dh || height > dh-dy)
        return dkr_vk_fail(g, "image copy rectangle exceeds storage");
    if (image_ready_read(g, source) || dkr_vk_image_ready(g, destination)) return -1;
    VkImageCopy regions[2]; unsigned n = 0;
    const VkImageAspectFlags aspects[] = {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_ASPECT_STENCIL_BIT};
    for (unsigned i = 0; i < 3; i++) if (source->info.aspect & aspects[i])
        regions[n++] = (VkImageCopy){.srcSubresource = {aspects[i], sl, sa, 1}, .srcOffset = {(int32_t)sx, (int32_t)sy, 0},
            .dstSubresource = {aspects[i], dl, da, 1}, .dstOffset = {(int32_t)dx, (int32_t)dy, 0}, .extent = {width, height, 1}};
    g->vkCmdCopyImage(g->command, source->image, VK_IMAGE_LAYOUT_GENERAL, destination->image, VK_IMAGE_LAYOUT_GENERAL, n, regions);
    destination->dirty = 1;
    return 0;
}

int dkr_vk_image_blit(DkrGfx *g, DkrVkImage *source, unsigned sl, unsigned sa, const int32_t from[4],
                      DkrVkImage *destination, unsigned dl, unsigned da, const int32_t to[4]) {
    if (!g || !g->batch_active || !source || !destination || source->owner != g || destination->owner != g) return -1;
    if (source == destination || source->info.aspect != destination->info.aspect ||
        sl >= source->info.levels || dl >= destination->info.levels || sa >= source->info.layers || da >= destination->info.layers)
        return dkr_vk_fail(g, "image blit needs distinct images with matching aspects");
    if (image_ready_read(g, source) || dkr_vk_image_ready(g, destination)) return -1;
    VkImageAspectFlags aspect = source->info.aspect;
    VkImageBlit region = {.srcSubresource = {aspect, sl, sa, 1}, .srcOffsets = {{from[0], from[1], 0}, {from[2], from[3], 1}},
                          .dstSubresource = {aspect, dl, da, 1}, .dstOffsets = {{to[0], to[1], 0}, {to[2], to[3], 1}}};
    g->vkCmdBlitImage(g->command, source->image, VK_IMAGE_LAYOUT_GENERAL, destination->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region,
                      aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    destination->dirty = 1;
    return 0;
}

static VkShaderStageFlagBits stage_flag(unsigned stage) {
    static const VkShaderStageFlagBits flags[] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_COMPUTE_BIT};
    return stage < DKR_VK_STAGES ? flags[stage] : 0;
}

static int shader_module(DkrGfx *g, const DkrVkShaderInfo *shader, VkShaderModule *out) {
    static const unsigned models[] = {0, 4, 3, 1, 2, 5};
    if (!shader->words || shader->bytes < 20 || shader->bytes % 4 || shader->words[0] != 0x07230203 || shader->stage >= DKR_VK_STAGES)
        return dkr_vk_fail(g, "invalid SPIR-V shader header/stage");
    uint32_t *words = malloc(shader->bytes);
    if (!words) return dkr_vk_fail(g, "out of memory copying SPIR-V");
    memcpy(words, shader->words, shader->bytes);
    size_t length = shader->bytes / 4;
    unsigned entries = 0;
    for (size_t p = 5; p < length;) {
        unsigned count = words[p] >> 16, op = words[p] & 65535;
        if (!count || count > length - p) { free(words); return dkr_vk_fail(g, "malformed SPIR-V instruction"); }
        if (op == 15) { /* OpEntryPoint */
            if (count < 4 || words[p + 1] != models[shader->stage]) { free(words); return dkr_vk_fail(g, "SPIR-V stage mismatch"); }
            entries++;
        }
        if (op == 71 && count == 4 && words[p + 2] == 34) /* OpDecorate DescriptorSet */
            words[p + 3] = shader->stage; /* NVN has independent descriptor namespaces per stage. */
        p += count;
    }
    if (entries != 1) { free(words); return dkr_vk_fail(g, "SPIR-V must have exactly one entry point"); }
    VkShaderModuleCreateInfo create = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = shader->bytes, .pCode = words};
    VkResult result = g->vkCreateShaderModule(g->device, &create, NULL, out);
    free(words);
    if (result != VK_SUCCESS) return dkr_vk_fail(g, "vkCreateShaderModule failed: %d", result);
    return g->validation_errors ? dkr_vk_fail(g, "shader module reported Vulkan validation errors") : 0;
}

int dkr_vk_program_create(DkrGfx *g, const DkrVkShaderInfo *shaders, unsigned count,
                          const DkrVkBinding *bindings, unsigned binding_count, DkrVkProgram **out) {
    if (!g || !out || !shaders || (binding_count && !bindings)) return -1;
    *out = NULL;
    if (!count || count > DKR_VK_STAGES || binding_count > DKR_VK_BINDINGS)
        return dkr_vk_fail(g, "invalid shader/binding count");
    DkrVkResources *r = resources(g);
    if (!r) return -1;
    DkrVkProgram *p = calloc(1, sizeof *p);
    if (!p) return dkr_vk_fail(g, "out of memory creating Vulkan program");
    p->owner = g; p->next = r->programs; r->programs = p; *out = p;
    if (r->properties.limits.maxBoundDescriptorSets < DKR_VK_STAGES)
        return dkr_vk_fail(g, "device does not support six independent NVN descriptor sets");
    p->binding_count = binding_count;
    if (binding_count) memcpy(p->bindings, bindings, sizeof *bindings * binding_count);
    for (unsigned i = 0; i < count; i++) {
        unsigned stage = shaders[i].stage;
        if (stage >= DKR_VK_STAGES || p->shaders[stage]) return dkr_vk_fail(g, "duplicate or invalid shader stage");
        if (shader_module(g, &shaders[i], &p->shaders[stage])) return -1;
    }
    for (unsigned stage = 0; stage < DKR_VK_STAGES; stage++) {
        VkDescriptorSetLayoutBinding entries[DKR_VK_BINDINGS];
        unsigned n = 0;
        for (unsigned i = 0; i < binding_count; i++) {
            const DkrVkBinding *b = &bindings[i];
            if (b->stage >= DKR_VK_STAGES || !p->shaders[b->stage] || b->count != 1)
                return dkr_vk_fail(g, "invalid stage or unsupported descriptor array");
            if (b->stage != stage) continue;
            for (unsigned j = 0; j < n; j++)
                if (entries[j].binding == b->binding) return dkr_vk_fail(g, "duplicate descriptor binding");
            entries[n++] = (VkDescriptorSetLayoutBinding){b->binding, b->type, b->count, stage_flag(stage), NULL};
        }
        VkDescriptorSetLayoutCreateInfo create = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = n, .pBindings = entries};
        VK_TRY(g, g->vkCreateDescriptorSetLayout(g->device, &create, NULL, &p->sets[stage]));
    }
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = DKR_VK_STAGES, .pSetLayouts = p->sets};
    VK_TRY(g, g->vkCreatePipelineLayout(g->device, &layout, NULL, &p->layout));
    return 0;
}

static int render_pass(DkrGfx *g, const DkrVkPipelineInfo *info, VkRenderPass *out) {
    DkrVkResources *r = resources(g);
    if (!r) return -1;
    for (RenderPass *rp = r->render_passes; rp; rp = rp->next) {
        if (rp->color_count == info->color_count && rp->depth == info->depth &&
            !memcmp(rp->colors, info->colors, info->color_count * sizeof *info->colors)) {
            *out = rp->pass;
            return 0;
        }
    }
    VkAttachmentDescription attachments[9] = {{0}};
    VkAttachmentReference colors[8], depth;
    unsigned n = 0;
    for (unsigned i = 0; i < info->color_count; i++) {
        if (info->colors[i] == VK_FORMAT_UNDEFINED) return dkr_vk_fail(g, "sparse color targets are not implemented");
        colors[i] = (VkAttachmentReference){n, VK_IMAGE_LAYOUT_GENERAL};
        attachments[n++] = (VkAttachmentDescription){.format = info->colors[i], .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    }
    if (info->depth != VK_FORMAT_UNDEFINED) {
        depth = (VkAttachmentReference){n, VK_IMAGE_LAYOUT_GENERAL};
        attachments[n++] = (VkAttachmentDescription){.format = info->depth, .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    }
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = info->color_count, .pColorAttachments = colors,
        .pDepthStencilAttachment = info->depth == VK_FORMAT_UNDEFINED ? NULL : &depth};
    VkRenderPassCreateInfo create = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = n, .pAttachments = attachments, .subpassCount = 1, .pSubpasses = &subpass};
    RenderPass *rp = calloc(1, sizeof *rp);
    if (!rp) return dkr_vk_fail(g, "out of render-pass memory");
    VK_TRY(g, g->vkCreateRenderPass(g->device, &create, NULL, &rp->pass));
    rp->color_count = info->color_count; rp->depth = info->depth;
    memcpy(rp->colors, info->colors, info->color_count * sizeof *info->colors);
    rp->next = r->render_passes; r->render_passes = rp;
    *out = rp->pass;
    return 0;
}

static int pipeline(DkrGfx *g, DkrVkProgram *program, const DkrVkPipelineInfo *info, Pipeline **out) {
    if (info->color_count > 8 || info->attribute_count > DKR_VK_VERTEX_SLOTS || info->stream_count > DKR_VK_VERTEX_SLOTS)
        return dkr_vk_fail(g, "pipeline arrays exceed backend limits");
    for (Pipeline *p = program->pipelines; p; p = p->next)
        if (p->pipeline && !memcmp(&p->info, info, sizeof *info)) { *out = p; return 0; }
    if (!program->shaders[0] || program->shaders[3] || program->shaders[4] || program->shaders[5])
        return dkr_vk_fail(g, "graphics pipeline requires VS and currently excludes tessellation/compute");
    if (info->depth_minus_w && !g->depth_clip_control)
        return dkr_vk_fail(g, "negative-one-to-one depth requires VK_EXT_depth_clip_control");
    Pipeline *p = calloc(1, sizeof *p);
    if (!p) return dkr_vk_fail(g, "out of memory creating a pipeline");
    p->info = *info; p->next = program->pipelines; program->pipelines = p; *out = p;
    DKR_PERF_ADD(pipelines, 1);
    if (render_pass(g, info, &p->pass)) return -1;
    VkPipelineShaderStageCreateInfo shaders[DKR_VK_STAGES];
    unsigned n = 0;
    for (unsigned i = 0; i < DKR_VK_STAGES; i++)
        if (program->shaders[i]) shaders[n++] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = stage_flag(i), .module = program->shaders[i], .pName = "main"};
    VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = info->stream_count, .pVertexBindingDescriptions = info->streams,
        .vertexAttributeDescriptionCount = info->attribute_count, .pVertexAttributeDescriptions = info->attributes};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = info->topology};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
        .negativeOneToOne = info->depth_minus_w};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = g->depth_clip_control ? &clip : NULL, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .rasterizerDiscardEnable = info->rasterizer_discard,
        .depthClampEnable = info->depth_clamp, .polygonMode = info->polygon, .cullMode = info->cull, .frontFace = info->front,
        .depthBiasEnable = info->depth_bias, .lineWidth = 1.f};
    VkPipelineMultisampleStateCreateInfo samples = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depth = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = info->depth_test, .depthWriteEnable = info->depth_write, .depthCompareOp = info->depth_compare,
        .stencilTestEnable = info->stencil_test, .front = info->stencil_front, .back = info->stencil_back};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = info->color_count, .pAttachments = info->blend};
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = sizeof states / sizeof *states, .pDynamicStates = states};
    VkGraphicsPipelineCreateInfo create = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = n, .pStages = shaders, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport, .pRasterizationState = &raster, .pMultisampleState = &samples,
        .pDepthStencilState = &depth, .pColorBlendState = &blend, .pDynamicState = &dynamic,
        .layout = program->layout, .renderPass = p->pass};
    VK_TRY(g, g->vkCreateGraphicsPipelines(g->device, VK_NULL_HANDLE, 1, &create, NULL, &p->pipeline));
    return g->validation_errors ? dkr_vk_fail(g, "graphics pipeline reported Vulkan validation errors") : 0;
}

static int bind_descriptors(DkrGfx *g, const DkrVkDraw *draw, VkDescriptorSet sets[DKR_VK_STAGES]) {
    DkrVkProgram *program = draw->program;
    if (draw->descriptor_count != program->binding_count || (draw->descriptor_count && !draw->descriptors))
        return dkr_vk_fail(g, "draw descriptor count does not match its program");
    VkDescriptorBufferInfo buffers[DKR_VK_BINDINGS];
    VkDescriptorImageInfo images[DKR_VK_BINDINGS];
    VkWriteDescriptorSet writes[DKR_VK_BINDINGS];
    for (unsigned i = 0; i < program->binding_count; i++) {
        const DkrVkBinding *b = &program->bindings[i];
        const DkrVkDescriptor *d = NULL;
        for (unsigned j = 0; j < draw->descriptor_count; j++)
            if (draw->descriptors[j].stage == b->stage && draw->descriptors[j].binding == b->binding) {
                if (d) return dkr_vk_fail(g, "duplicate draw descriptor");
                d = &draw->descriptors[j];
            }
        if (!d) return dkr_vk_fail(g, "draw is missing descriptor %u:%u", b->stage, b->binding);
        if (!sets[b->stage]) {
            VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = g->resources->descriptors, .descriptorSetCount = 1, .pSetLayouts = &program->sets[b->stage]};
            VK_TRY(g, g->vkAllocateDescriptorSets(g->device, &allocate, &sets[b->stage]));
        }
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = sets[b->stage], .dstBinding = b->binding, .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            if (!d->buffer || d->buffer->owner != g || !d->size || d->offset > d->buffer->size || d->size > d->buffer->size - d->offset)
                return dkr_vk_fail(g, "invalid buffer descriptor range");
            const VkPhysicalDeviceLimits *limits = &g->resources->properties.limits;
            uint64_t align = b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? limits->minUniformBufferOffsetAlignment : limits->minStorageBufferOffsetAlignment;
            uint64_t max = b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? limits->maxUniformBufferRange : limits->maxStorageBufferRange;
            if ((align && (d->buffer->base + d->offset) % align) || d->size > max) return dkr_vk_fail(g, "buffer descriptor violates device alignment/range");
            buffers[i] = (VkDescriptorBufferInfo){d->buffer->buffer, d->buffer->base + d->offset, d->size};
            writes[i].pBufferInfo = &buffers[i];
        } else {
            int need_image = b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            int need_sampler = b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b->type == VK_DESCRIPTOR_TYPE_SAMPLER;
            if (!need_image && !need_sampler) return dkr_vk_fail(g, "descriptor type %d is not implemented", b->type);
            if ((need_sampler && (!d->sampler || d->sampler->owner != g)) || (need_image && (!d->image || d->image->owner != g)))
                return dkr_vk_fail(g, "invalid image/sampler descriptor");
            DkrVkImage *src_image = d->image;
            DkrVkView *src_view = d->view;
            if (need_image) {
                int feedback = 0;
                for (unsigned j = 0; j < draw->pipeline.color_count; j++) {
                    if (draw->colors[j] == src_image) {
                        DkrVkView *tv = draw->color_views[j], *sv = src_view;
                        unsigned tbl = tv ? tv->info.base_level : 0, tba = tv ? tv->info.base_layer : 0;
                        unsigned sbl = sv ? sv->info.base_level : 0, sba = sv ? sv->info.base_layer : 0;
                        unsigned slv = sv ? sv->info.levels : src_image->info.levels, sla = sv ? sv->info.layers : src_image->info.layers;
                        if (tbl >= sbl && tbl < sbl + slv && tba >= sba && tba < sba + sla) {
                            feedback = 1; break;
                        }
                    }
                }
                if (draw->depth == src_image) {
                    DkrVkView *tv = draw->depth_view, *sv = src_view;
                    unsigned tbl = tv ? tv->info.base_level : 0, tba = tv ? tv->info.base_layer : 0;
                    unsigned sbl = sv ? sv->info.base_level : 0, sba = sv ? sv->info.base_layer : 0;
                    unsigned slv = sv ? sv->info.levels : src_image->info.levels, sla = sv ? sv->info.layers : src_image->info.layers;
                    if (tbl >= sbl && tbl < sbl + slv && tba >= sba && tba < sba + sla) feedback = 1;
                }
                if (feedback) {
                    DkrVkResources *r = g->resources;
                    if (!r->snapshot || memcmp(&r->snapshot->info, &src_image->info, sizeof src_image->info) != 0) {
                        DkrVkImageInfo snap_info = src_image->info;
                        if (dkr_vk_image_create(g, &snap_info, &r->snapshot)) return -1;
                    }
                    DkrVkImage *snap = r->snapshot;
                    for (unsigned lyr = 0; lyr < src_image->info.layers; lyr++) {
                        for (unsigned lvl = 0; lvl < src_image->info.levels; lvl++) {
                            unsigned w = src_image->info.width >> lvl, h = src_image->info.height >> lvl;
                            if (!w) w = 1; if (!h) h = 1;
                            if (dkr_vk_image_copy(g, src_image, lvl, lyr, 0, 0, snap, lvl, lyr, 0, 0, w, h)) return -1;
                        }
                    }
                    if (dkr_vk_image_ready(g, snap)) return -1;
                    src_image = snap;
                    if (src_view) {
                        DkrVkView *snap_view = NULL;
                        if (dkr_vk_image_view(g, snap, &src_view->info, &snap_view)) return -1;
                        src_view = snap_view;
                    }
                }
                if (image_ready_read(g, src_image)) return -1;
            }
            if (src_view && (!need_image || src_view->image != src_image)) return dkr_vk_fail(g, "descriptor view belongs to another image");
            images[i] = (VkDescriptorImageInfo){need_sampler ? d->sampler->sampler : VK_NULL_HANDLE,
                need_image ? (src_view ? src_view->view : src_image->view) : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            writes[i].pImageInfo = &images[i];
        }
    }
    g->vkUpdateDescriptorSets(g->device, program->binding_count, writes, 0, NULL);
    return 0;
}

int dkr_vk_draw(DkrGfx *g, const DkrVkDraw *draw) {
    if (!g || !g->batch_active || !draw || !draw->program || draw->program->owner != g) return -1;
    if (!draw->count || !draw->instances) return 0;
    const DkrVkPipelineInfo *info = &draw->pipeline;
    if (info->color_count > 8 || (!info->color_count && !draw->depth && !info->rasterizer_discard)) return dkr_vk_fail(g, "draw has no valid render targets");
    if (info->stream_count > DKR_VK_VERTEX_SLOTS || info->attribute_count > DKR_VK_VERTEX_SLOTS)
        return dkr_vk_fail(g, "vertex layout exceeds backend limits");
    uint32_t width = UINT32_MAX, height = UINT32_MAX;
    VkImageView views[9];
    unsigned view_count = 0;
    int capturing = 0;
    for (unsigned i = 0; i < 4; i++) if (draw->feedback[i]) {
        DkrVkBuffer *b = draw->feedback[i]; capturing = 1;
        if (!g->transform_feedback || b->owner != g || b->temporary || !draw->feedback_sizes[i] ||
            draw->feedback_offsets[i] % 4 || draw->feedback_offsets[i] > b->size ||
            draw->feedback_sizes[i] > b->size - draw->feedback_offsets[i])
            return dkr_vk_fail(g, "invalid transform feedback buffer/range");
    }
    if (!info->color_count && !draw->depth) width = height = 1;
    for (unsigned i = 0; i < info->color_count + (draw->depth != NULL); i++) {
        int depth = i == info->color_count;
        DkrVkImage *image = depth ? draw->depth : draw->colors[i];
        DkrVkView *view = depth ? draw->depth_view : draw->color_views[i];
        VkImageUsageFlags usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkFormat format = depth ? info->depth : info->colors[i];
        if (!image || image->owner != g || (view ? view->info.format : image->info.format) != format ||
            (view && view->image != image) || !(image->usage & usage))
            return dkr_vk_fail(g, "render target does not match its pipeline");
        unsigned iw = image->info.width >> (view ? view->info.base_level : 0), ih = image->info.height >> (view ? view->info.base_level : 0);
        if (!iw) iw = 1; if (!ih) ih = 1;
        if (iw < width) width = iw; if (ih < height) height = ih;
        views[view_count++] = view ? view->view : image->target_view;
    }
    if ((draw->depth == NULL) != (info->depth == VK_FORMAT_UNDEFINED)) return dkr_vk_fail(g, "missing depth target or format");
    if (!(draw->viewport.width > 0) || draw->viewport.height == 0 || draw->scissor.offset.x < 0 || draw->scissor.offset.y < 0 ||
        (uint64_t)draw->scissor.offset.x + draw->scissor.extent.width > width ||
        (uint64_t)draw->scissor.offset.y + draw->scissor.extent.height > height)
        return dkr_vk_fail(g, "invalid viewport/scissor for target extent");
    for (unsigned i = 0; i < info->stream_count; i++) {
        unsigned binding = info->streams[i].binding;
        if (binding >= DKR_VK_VERTEX_SLOTS || !draw->vertices[binding] || draw->vertices[binding]->owner != g ||
            draw->vertex_offsets[binding] >= draw->vertices[binding]->size)
            return dkr_vk_fail(g, "invalid vertex buffer binding");
    }
    if (draw->indices) {
        uint64_t bytes = draw->index_type == VK_INDEX_TYPE_UINT16 ? 2 : draw->index_type == VK_INDEX_TYPE_UINT32 ? 4 : 0;
        uint64_t end = ((uint64_t)draw->first + draw->count) * bytes;
        if (!bytes || draw->indices->owner != g || draw->index_offset % bytes || draw->index_offset > draw->indices->size ||
            end > draw->indices->size - draw->index_offset) return dkr_vk_fail(g, "invalid index buffer range/type");
    }
    Pipeline *p;
    if (pipeline(g, draw->program, info, &p)) return -1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (get_framebuffer(g, p->pass, view_count, views, width, height, &fb)) return -1;
    DkrVkResources *r = g->resources;
    if (r->active_framebuffer != fb || capturing) end_render_pass(g);
    VkDescriptorSet sets[DKR_VK_STAGES] = {0};
    if (bind_descriptors(g, draw, sets)) return -1;
    if (r->active_framebuffer != fb) {
        end_render_pass(g);
        for (unsigned i = 0; i < info->color_count + (draw->depth != NULL); i++) {
            DkrVkImage *image = (i == info->color_count) ? draw->depth : draw->colors[i];
            if (dkr_vk_image_ready(g, image)) return -1;
            image->dirty = 1;
        }
        VkRenderPassBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = p->pass,
            .framebuffer = fb, .renderArea = {{0, 0}, {width, height}}};
        g->vkCmdBeginRenderPass(g->command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        r->active_pass = p->pass;
        r->active_framebuffer = fb;
    }
    g->vkCmdBindPipeline(g->command, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    if (capturing) {
        for (unsigned i = 0; i < 4; i++) if (draw->feedback[i]) {
            VkBuffer buffer = draw->feedback[i]->buffer;
            g->vkCmdBindTransformFeedbackBuffersEXT(g->command, i, 1, &buffer, &draw->feedback_offsets[i], &draw->feedback_sizes[i]);
        }
        g->vkCmdBeginTransformFeedbackEXT(g->command, 0, 0, NULL, NULL);
    }
    for (unsigned i = 0; i < DKR_VK_STAGES; i++)
        if (sets[i]) g->vkCmdBindDescriptorSets(g->command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw->program->layout, i, 1, &sets[i], 0, NULL);
    if (!info->rasterizer_discard) {
        g->vkCmdSetViewport(g->command, 0, 1, &draw->viewport);
        g->vkCmdSetScissor(g->command, 0, 1, &draw->scissor);
        g->vkCmdSetBlendConstants(g->command, draw->blend_constant);
        g->vkCmdSetDepthBias(g->command, draw->depth_bias_constant, draw->depth_bias_clamp, draw->depth_bias_slope);
        g->vkCmdSetStencilReference(g->command, VK_STENCIL_FACE_FRONT_BIT, info->stencil_front.reference);
        g->vkCmdSetStencilReference(g->command, VK_STENCIL_FACE_BACK_BIT, info->stencil_back.reference);
        g->vkCmdSetStencilCompareMask(g->command, VK_STENCIL_FACE_FRONT_BIT, info->stencil_front.compareMask);
        g->vkCmdSetStencilCompareMask(g->command, VK_STENCIL_FACE_BACK_BIT, info->stencil_back.compareMask);
        g->vkCmdSetStencilWriteMask(g->command, VK_STENCIL_FACE_FRONT_BIT, info->stencil_front.writeMask);
        g->vkCmdSetStencilWriteMask(g->command, VK_STENCIL_FACE_BACK_BIT, info->stencil_back.writeMask);
    }
    for (unsigned i = 0; i < info->stream_count; i++) {
        unsigned binding = info->streams[i].binding;
        VkDeviceSize offset = draw->vertices[binding]->base + draw->vertex_offsets[binding];
        g->vkCmdBindVertexBuffers(g->command, binding, 1, &draw->vertices[binding]->buffer, &offset);
    }
    if (draw->indices) {
        g->vkCmdBindIndexBuffer(g->command, draw->indices->buffer, draw->indices->base + draw->index_offset, draw->index_type);
        g->vkCmdDrawIndexed(g->command, draw->count, draw->instances, draw->first, draw->base_vertex, draw->first_instance);
    } else g->vkCmdDraw(g->command, draw->count, draw->instances, draw->first, draw->first_instance);
    if (capturing) {
        g->vkCmdEndTransformFeedbackEXT(g->command, 0, 0, NULL, NULL);
        end_render_pass(g);
        VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        g->vkCmdPipelineBarrier(g->command, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &barrier, 0, NULL, 0, NULL);
    }
    return 0;
}

void dkr_vk_resources_destroy(DkrGfx *g) {
    DkrVkResources *r = g->resources;
    if (!r) return;
    /* Teardown is also valid after a recording error. vk.c has already waited for the device. */
    if (g->batch_active) g->vkResetCommandBuffer(g->command, 0);
    g->batch_active = 0;
    r->active_pass = VK_NULL_HANDLE;
    r->active_framebuffer = VK_NULL_HANDLE;
    recycle(g);
    destroy_framebuffers(g);
    while (r->free_temp_buffers) {
        DkrVkBuffer *b = r->free_temp_buffers; r->free_temp_buffers = b->next; free(b);
    }
    while (r->buffers) { DkrVkBuffer *b = r->buffers; r->buffers = b->next; destroy_buffer(g, b); }
    while (r->chunks) { Chunk *k = r->chunks; r->chunks = k->next; chunk_destroy(g, k); }
    while (r->images) dkr_vk_image_destroy(g, r->images);
    while (r->samplers) {
        DkrVkSampler *s = r->samplers; r->samplers = s->next;
        if (s->sampler) g->vkDestroySampler(g->device, s->sampler, NULL);
        free(s);
    }
    while (r->programs) {
        DkrVkProgram *p = r->programs; r->programs = p->next;
        while (p->pipelines) {
            Pipeline *v = p->pipelines; p->pipelines = v->next;
            if (v->pipeline) g->vkDestroyPipeline(g->device, v->pipeline, NULL);
            free(v);
        }
        for (unsigned i = 0; i < DKR_VK_STAGES; i++) {
            if (p->shaders[i]) g->vkDestroyShaderModule(g->device, p->shaders[i], NULL);
            if (p->sets[i]) g->vkDestroyDescriptorSetLayout(g->device, p->sets[i], NULL);
        }
        if (p->layout) g->vkDestroyPipelineLayout(g->device, p->layout, NULL);
        free(p);
    }
    while (r->render_passes) {
        RenderPass *rp = r->render_passes; r->render_passes = rp->next;
        if (rp->pass) g->vkDestroyRenderPass(g->device, rp->pass, NULL);
        free(rp);
    }
    if (r->descriptors) g->vkDestroyDescriptorPool(g->device, r->descriptors, NULL);
    free(r); g->resources = NULL;
}
