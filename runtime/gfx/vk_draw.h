/* Vulkan resources and draw batches, independent of guest CPU/NVN object layouts. */
#ifndef DKR_VK_DRAW_H
#define DKR_VK_DRAW_H
#include "dkr/gfx.h"
#include <vulkan/vulkan.h>

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
} DkrVkDraw;

/* Resources belong to gfx and are also reclaimed by dkr_gfx_destroy. Destroy explicit resources only
 * outside a batch; all batches currently wait for completion, including before transient storage is recycled.
 * A recording error requires context teardown; image layout tracking is not rolled back for a failed batch. */
int dkr_vk_image_create(DkrGfx *gfx, const DkrVkImageInfo *info, DkrVkImage **out);
int dkr_vk_image_view(DkrGfx *gfx, DkrVkImage *image, const DkrVkViewInfo *info, DkrVkView **out);
void dkr_vk_image_destroy(DkrGfx *gfx, DkrVkImage *image);
int dkr_vk_buffer_create(DkrGfx *gfx, const void *data, size_t size, DkrVkBuffer **out);
int dkr_vk_buffer_temporary(DkrGfx *gfx, const void *data, size_t size, DkrVkBuffer **out);
void dkr_vk_buffer_destroy(DkrGfx *gfx, DkrVkBuffer *buffer);
int dkr_vk_sampler_create(DkrGfx *gfx, const VkSamplerCreateInfo *info, DkrVkSampler **out);
int dkr_vk_program_create(DkrGfx *gfx, const DkrVkShaderInfo *shaders, unsigned count,
                          const DkrVkBinding *bindings, unsigned binding_count, DkrVkProgram **out);
int dkr_vk_batch_begin(DkrGfx *gfx);
int dkr_vk_batch_end(DkrGfx *gfx);
int dkr_vk_image_upload(DkrGfx *gfx, DkrVkImage *image, unsigned level, unsigned layer, const void *data, size_t size);
int dkr_vk_image_upload_region(DkrGfx *gfx, DkrVkImage *image, unsigned level, unsigned layer,
                                unsigned x, unsigned y, unsigned width, unsigned height, const void *data, size_t size);
int dkr_vk_image_clear(DkrGfx *gfx, DkrVkImage *image, const float color[4]);
int dkr_vk_image_clear_depth(DkrGfx *gfx, DkrVkImage *image, VkImageAspectFlags aspects, float depth, uint32_t stencil);
int dkr_vk_image_copy(DkrGfx *gfx, DkrVkImage *source, unsigned source_level, unsigned source_layer, unsigned sx, unsigned sy,
                      DkrVkImage *destination, unsigned destination_level, unsigned destination_layer, unsigned dx, unsigned dy,
                      unsigned width, unsigned height);
int dkr_vk_draw(DkrGfx *gfx, const DkrVkDraw *draw);
int dkr_vk_image_present(DkrGfx *gfx, DkrVkImage *image, const char *capture_bmp);

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
    int initialized;
};
struct DkrVkView { DkrVkView *next; DkrVkImage *image; DkrVkViewInfo info; VkImageView view; };
struct DkrVkBuffer {
    DkrVkBuffer *next;
    DkrGfx *owner;
    VkBuffer buffer;
    VkDeviceMemory memory;
    size_t size;
    int temporary;
};
void dkr_vk_resources_destroy(DkrGfx *gfx);
int dkr_vk_image_ready(DkrGfx *gfx, DkrVkImage *image);
#endif
