/* Vulkan resources and draw batches, independent of guest CPU/NVN object layouts. */
#ifndef DKR_VK_DRAW_H
#define DKR_VK_DRAW_H
#include "dkr/gfx.h"
#include <vulkan/vulkan.h>
#include <wchar.h>

#define DKR_VK_STAGES 6
#define DKR_VK_BINDINGS 128
#define DKR_VK_VERTEX_SLOTS 16

typedef struct DkrVkImage DkrVkImage;
typedef struct DkrVkBuffer DkrVkBuffer;
typedef struct DkrVkProgram DkrVkProgram;
typedef struct DkrVkSampler DkrVkSampler;
typedef struct DkrVkView DkrVkView;
typedef struct DkrVkViewInfo {
    VkFormat format;
    VkImageAspectFlags aspects;
    VkImageViewType type;
    uint32_t base_level, levels, base_layer, layers;
    VkComponentMapping components;
} DkrVkViewInfo;

typedef struct DkrVkImageInfo {
    VkFormat format;
    uint32_t width, height, levels, layers;
    VkImageAspectFlags aspect;
} DkrVkImageInfo;

typedef struct DkrVkShaderInfo {
    unsigned stage; /* NVN order: VS, PS, GS, TCS, TES, CS; each receives its own descriptor set */
    const uint32_t *words;
    size_t bytes;
} DkrVkShaderInfo;

typedef struct DkrVkBinding {
    uint32_t stage, binding;
    VkDescriptorType type;
    uint32_t count;
} DkrVkBinding;

typedef struct DkrVkPipelineInfo {
    VkPrimitiveTopology topology;
    uint32_t attribute_count, stream_count, color_count;
    VkVertexInputAttributeDescription attributes[DKR_VK_VERTEX_SLOTS];
    VkVertexInputBindingDescription streams[DKR_VK_VERTEX_SLOTS];
    VkFormat colors[8], depth;
    VkPipelineColorBlendAttachmentState blend[8];
    VkCullModeFlags cull;
    VkFrontFace front;
    VkPolygonMode polygon;
    VkBool32 depth_test, depth_write, depth_clamp, depth_minus_w, depth_bias, stencil_test;
    VkBool32 rasterizer_discard;
    VkCompareOp depth_compare;
    VkStencilOpState stencil_front, stencil_back;
} DkrVkPipelineInfo;

typedef struct DkrVkDescriptor {
    uint32_t stage, binding;
    DkrVkBuffer *buffer;
    uint64_t offset, size;
    DkrVkImage *image;
    DkrVkView *view;
    DkrVkSampler *sampler;
} DkrVkDescriptor;

typedef struct DkrVkDraw {
    DkrVkProgram *program;
    DkrVkPipelineInfo pipeline;
    DkrVkImage *colors[8], *depth;
    DkrVkView *color_views[8], *depth_view;
    VkViewport viewport;
    VkRect2D scissor;
    float blend_constant[4], depth_bias_constant, depth_bias_slope, depth_bias_clamp;
    DkrVkBuffer *vertices[DKR_VK_VERTEX_SLOTS], *indices;
    VkDeviceSize vertex_offsets[DKR_VK_VERTEX_SLOTS], index_offset;
    VkIndexType index_type;
    const DkrVkDescriptor *descriptors;
    uint32_t descriptor_count;
    uint32_t count, first, instances, first_instance;
    int32_t base_vertex;
    DkrVkBuffer *feedback[4];
    VkDeviceSize feedback_offsets[4], feedback_sizes[4];
} DkrVkDraw;

/* Resources belong to gfx and are also reclaimed by dkr_gfx_destroy. Destroy explicit resources only
 * outside a batch; all batches currently wait for completion, including before transient storage is recycled.
 * A recording error requires context teardown; image layout tracking is not rolled back for a failed batch. */
int dkr_vk_image_create(DkrGfx *gfx, const DkrVkImageInfo *info, DkrVkImage **out);
int dkr_vk_image_view(DkrGfx *gfx, DkrVkImage *image, const DkrVkViewInfo *info, DkrVkView **out);
void dkr_vk_image_destroy(DkrGfx *gfx, DkrVkImage *image);
int dkr_vk_buffer_create(DkrGfx *gfx, const void *data, size_t size, DkrVkBuffer **out);
int dkr_vk_buffer_temporary(DkrGfx *gfx, const void *data, size_t size, DkrVkBuffer **out);
int dkr_vk_buffer_read(DkrGfx *gfx, DkrVkBuffer *buffer, size_t offset, void *out, size_t size);
int dkr_vk_feedback_supported(DkrGfx *gfx);
void dkr_vk_buffer_destroy(DkrGfx *gfx, DkrVkBuffer *buffer);
int dkr_vk_sampler_create(DkrGfx *gfx, const VkSamplerCreateInfo *info, DkrVkSampler **out);
int dkr_vk_program_create(DkrGfx *gfx, const DkrVkShaderInfo *shaders, unsigned count,
                          const DkrVkBinding *bindings, unsigned binding_count, DkrVkProgram **out);
int dkr_vk_batch_begin(DkrGfx *gfx);
int dkr_vk_batch_resume(DkrGfx *gfx);
int dkr_vk_batch_end(DkrGfx *gfx);
int dkr_vk_image_upload(DkrGfx *gfx, DkrVkImage *image, unsigned level, unsigned layer, const void *data, size_t size);
int dkr_vk_image_upload_region(DkrGfx *gfx, DkrVkImage *image, unsigned level, unsigned layer,
                                unsigned x, unsigned y, unsigned width, unsigned height, const void *data, size_t size);
int dkr_vk_image_clear(DkrGfx *gfx, DkrVkImage *image, const float color[4]);
int dkr_vk_image_clear_depth(DkrGfx *gfx, DkrVkImage *image, VkImageAspectFlags aspects, float depth, uint32_t stencil);
/* Clear exactly one attachment mip/layer and a clipped rectangle, preserving all other texels/aspects. */
int dkr_vk_attachment_clear(DkrGfx *gfx, DkrVkImage *image, DkrVkView *view, VkRect2D rect,
                            VkImageAspectFlags aspects, const VkClearValue *value);
int dkr_vk_image_copy(DkrGfx *gfx, DkrVkImage *source, unsigned source_level, unsigned source_layer, unsigned sx, unsigned sy,
                      DkrVkImage *destination, unsigned destination_level, unsigned destination_layer, unsigned dx, unsigned dy,
                      unsigned width, unsigned height);
/* Scaled copy of one color/depth subresource rectangle into another (vkCmdBlitImage; linear for color). */
int dkr_vk_image_blit(DkrGfx *gfx, DkrVkImage *source, unsigned source_level, unsigned source_layer, const int32_t source_rect[4],
                      DkrVkImage *destination, unsigned destination_level, unsigned destination_layer, const int32_t destination_rect[4]);
int dkr_vk_draw(DkrGfx *gfx, const DkrVkDraw *draw);
/* Persistent driver pipeline cache. open loads `path` when its header matches this device (otherwise starts empty)
 * and makes every later pipeline use the cache. save writes it back atomically when new pipelines were added;
 * with `when_idle` it only does so after a few seconds without a new pipeline, so it never lands on a busy frame.
 * dkr_gfx_destroy saves it one last time. Both return 0 on success and also when nothing had to be done. */
int dkr_vk_pipeline_cache_open(DkrGfx *gfx, const wchar_t *path);
int dkr_vk_pipeline_cache_save(DkrGfx *gfx, int when_idle);
int dkr_vk_image_present(DkrGfx *gfx, DkrVkImage *image, const char *capture_bmp);
/* Presented images are stored bottom-up (lower-left window origin): flip them vertically on the way to the window. */
void dkr_gfx_set_present_flip(DkrGfx *gfx, int flip);

/* Backend implementation details shared with vk.c; callers use the opaque resource pointers above. */
struct DkrVkImage {
    DkrVkImage *next;
    DkrGfx *owner;
    DkrVkImageInfo info;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkImageView target_view;
    VkImageUsageFlags usage;
    DkrVkView *views;
    int initialized, dirty;
};
struct DkrVkView { DkrVkView *next; DkrVkImage *image; DkrVkViewInfo info; VkImageView view; };
struct DkrVkBuffer {
    DkrVkBuffer *next;
    DkrGfx *owner;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    int coherent;
    size_t size;
    int temporary;      /* 1: a slice of the batch upload arena, released when the next batch begins */
    VkDeviceSize base;  /* byte offset of this buffer inside `buffer` (non-zero only for arena slices) */
};
void dkr_vk_resources_destroy(DkrGfx *gfx);
int dkr_vk_image_ready(DkrGfx *gfx, DkrVkImage *image);
#endif
