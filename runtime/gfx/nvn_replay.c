/* Consume immutable NVN records on the actual Vulkan backend. Unsupported rendering operations stop explicitly. */
#include "nvn_replay.h"
#include "nvn_internal.h"
#include "nvn_commands.h"
#include "shader_client.h"
#include "vk_draw.h"
#include "astc.h"
#include "perf.h"
#include "dkr/settings.h"
#include "../src/hos.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PTR(x) ((void *)(uintptr_t)(x))
#define REQUIRE(x) do { if ((x) != 0) hos_stop(s->cpu, "NVN replay: %s", dkr_gfx_error(s->gfx)); } while (0)
typedef struct Range { uint64_t address, size; } Range;
typedef struct FeedbackBuffer { struct FeedbackBuffer *next; Range range; DkrVkBuffer *buffer; } FeedbackBuffer;
static FeedbackBuffer *feedback_buffers;
typedef struct Image {
    struct Image *next, *hash_next;
    NvnTextureInfo texture;
    DkrVkImage *image;
    int uploaded, rendered;
    float scale; /* render_scale applied to this image's storage (render targets only), 1 otherwise */
} Image;
/* render_scale (dkr/settings.h): render targets are created this much larger, see target(). */
static float g_render_scale = 1;
typedef struct Staging { struct Staging *next; DkrVkImage *image; } Staging; /* unscaled stand-ins for CPU uploads into scaled targets */
static Staging *stagings;
typedef struct Sampler { struct Sampler *next; uint8_t builder[0x48]; DkrVkSampler *sampler; } Sampler;
typedef struct StageInfo {
    struct StageInfo *next;
    char key[33];
    uint32_t used_tex_mask;
    int has_mask;
} StageInfo;
typedef struct StageEntry {
    struct StageEntry *next, *hash_next;
    char key[33];
    NvnShaderInputs inputs;
    DkrShaderCache cache;
    uint32_t id;
} StageEntry;
typedef struct Program {
    struct Program *next, *hash_next;
    uint32_t stage_ids[6];
    DkrVkProgram *program;
    DkrShaderCache shaders[6];
} Program;
typedef struct Replay {
    Cpu *cpu;
    DkrGfx *gfx;
    uint64_t device, texture_pool, sampler_pool, programs[6], textures[6][32];
    Range uniforms[6][16], vertices[16];
    uint8_t attributes[16][4], streams[16][8];
    unsigned attribute_count, stream_count, target_count;
    NvnTextureInfo colors[8], depth;
    int has_depth, has_viewport, has_scissor;
    float viewport[4], depth_range[2], blend_color[4], bias[3];
    int32_t scissor[4];
    uint32_t blend_enable[8], write_mask[8], stencil_mask[2], stencil_value_mask[2], stencil_ref[2];
    VkPipelineColorBlendAttachmentState blend[8];
    VkBool32 depth_test, depth_write, stencil_test, depth_clamp, depth_bias;
    VkCompareOp depth_compare;
    VkStencilOpState front, back;
    VkCullModeFlags cull;
    VkFrontFace winding;
    VkPolygonMode polygon;
    uint64_t draws;
    /* Guest vertex/index/uniform ranges already copied into this batch. Draws of one submission usually bind the same
     * large pool range; copying it once per batch instead of once per draw is what keeps the menu and gameplay fast.
     * Cleared at each batch; range-invalidated when the replay writes guest memory (UpdateUniformBuffer, XFB). */
    struct { uint64_t address, size; DkrVkBuffer *buffer; } uploaded[256];
    unsigned uploaded_count, uploaded_next;
    uint8_t last_support[1408];
    DkrVkBuffer *support_buffer;
    float support_w, support_h;
    int has_support;
    int xfb_active, discard; /* transform feedback between Begin/End (not paused); SetRasterizerDiscard */
    Range feedback[4];
    uint64_t feedback_written[4];
} Replay;
static uint64_t g_xfb_skipped; /* draws skipped because transform feedback is not emulated */
static Image *images;
static Image *image_buckets[4096];
static Sampler *samplers;
static StageInfo *stage_info_buckets[512];
static StageEntry *stage_entries;
static StageEntry *stage_buckets[1024];
static uint32_t next_stage_id;
static Program *programs;
static Program *program_buckets[1024];
static SRWLOCK lock = SRWLOCK_INIT;
static int g_trace_draws; /* set while the frame named by DKR_TRACE_DRAWS is being replayed */
static int g_lower_left;  /* NVN window origin of the last draw; the presented image is then stored bottom-up */

static uint32_t u32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float f32(const void *p) { float v; memcpy(&v, p, 4); return v; }
static uint64_t get(Replay *s, const char *name, const void *object, uint64_t a, uint64_t b, uint64_t d, uint64_t e, uint64_t f) {
    uint64_t address = dkr_nvn_original(name);
    if (!address) hos_stop(s->cpu, "NVN replay: original pure getter %s is unavailable", name);
    Cpu work = *s->cpu;
    work.x[0] = (uint64_t)(uintptr_t)object; work.x[1] = a; work.x[2] = b; work.x[3] = d; work.x[4] = e; work.x[5] = f;
    rt_call(&work, address);
    return work.x[0];
}

static VkFormat format(Replay *s, uint32_t f, int astc) {
    if (f >= 0x79 && f <= 0x94 && astc) return f >= 0x87 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    switch (f) {
    case 1: return VK_FORMAT_R8_UNORM; case 2: return VK_FORMAT_R8_SNORM;
    case 3: return VK_FORMAT_R8_UINT; case 4: return VK_FORMAT_R8_SINT;
    case 5: return VK_FORMAT_R16_SFLOAT; case 6: return VK_FORMAT_R16_UNORM; case 7: return VK_FORMAT_R16_SNORM;
    case 8: return VK_FORMAT_R16_UINT; case 9: return VK_FORMAT_R16_SINT;
    case 0xa: return VK_FORMAT_R32_SFLOAT; case 0xb: return VK_FORMAT_R32_UINT; case 0xc: return VK_FORMAT_R32_SINT;
    case 0xd: return VK_FORMAT_R8G8_UNORM; case 0xe: return VK_FORMAT_R8G8_SNORM;
    case 0xf: return VK_FORMAT_R8G8_UINT; case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT; case 0x12: return VK_FORMAT_R16G16_UNORM; case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT; case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT; case 0x17: return VK_FORMAT_R32G32_UINT; case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM; case 0x1a: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1b: return VK_FORMAT_R8G8B8_UINT; case 0x1c: return VK_FORMAT_R8G8B8_SINT;
    case 0x1d: return VK_FORMAT_R16G16B16_SFLOAT; case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM; case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT; case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT; case 0x2a: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2b: return VK_FORMAT_R16G16B16A16_SNORM; case 0x2c: return VK_FORMAT_R16G16B16A16_UINT; case 0x2d: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2e: return VK_FORMAT_R32G32B32A32_SFLOAT; case 0x2f: return VK_FORMAT_R32G32B32A32_UINT; case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x31: return VK_FORMAT_S8_UINT; case 0x32: return VK_FORMAT_D16_UNORM;
    case 0x33: case 0x34: return VK_FORMAT_D32_SFLOAT; case 0x35: case 0x36: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 0x38: return VK_FORMAT_R8G8B8A8_SRGB; case 0x3d: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x3e: return VK_FORMAT_A2B10G10R10_UINT_PACK32; case 0x3f: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x41: return VK_FORMAT_BC1_RGB_UNORM_BLOCK; case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK; case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x45: return VK_FORMAT_BC1_RGB_SRGB_BLOCK; case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK; case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK; case 0x4a: return VK_FORMAT_BC4_SNORM_BLOCK;
    case 0x4b: return VK_FORMAT_BC5_UNORM_BLOCK; case 0x4c: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x4d: return VK_FORMAT_BC7_UNORM_BLOCK; case 0x4e: return VK_FORMAT_BC7_SRGB_BLOCK;
    case 0x4f: return VK_FORMAT_BC6H_SFLOAT_BLOCK; case 0x50: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    default: hos_stop(s->cpu, "NVN replay: format %#x needs a Vulkan mapping", f); return VK_FORMAT_UNDEFINED;
    }
}
static unsigned image_hash(const NvnTextureInfo *t) {
    uint64_t h = t->address ^ ((uint64_t)t->desc.width << 32) ^ ((uint64_t)t->desc.height << 16) ^ t->desc.format;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;
    return (unsigned)(h & 4095);
}
static Image *find_image(const NvnTextureInfo *t) {
    Image *entry = image_buckets[image_hash(t)];
    while (entry && (entry->texture.address != t->address || memcmp(&entry->texture.desc, &t->desc, sizeof t->desc))) entry = entry->hash_next;
    return entry;
}
static Image *image_scaled(Replay *s, const NvnTextureInfo *t, int sample, float scale);
static Image *image(Replay *s, const NvnTextureInfo *t, int sample) { return image_scaled(s, t, sample, 1); }
/* A render target: created at `scale` when it does not exist yet (2D, single layer, at least 128x64 texels). */
static Image *target(Replay *s, const NvnTextureInfo *t, float scale) { return image_scaled(s, t, 0, scale); }
static Image *image_scaled(Replay *s, const NvnTextureInfo *t, int sample, float scale) {
    if (t->desc.target != 1 && t->desc.target != 4 && t->desc.target != 7 && t->desc.target != 8)
        hos_stop(s->cpu, "NVN replay: texture target %u needs image/view support", t->desc.target);
    if (t->desc.samples > 1) hos_stop(s->cpu, "NVN replay: multisampled image requires explicit support");
    Image *entry = find_image(t);
    if (!entry) {
        entry = calloc(1, sizeof *entry);
        if (!entry) hos_stop(s->cpu, "NVN replay: out of image-cache memory");
        unsigned h = image_hash(t);
        entry->texture = *t; entry->next = images; images = entry;
        entry->hash_next = image_buckets[h]; image_buckets[h] = entry;
        if (scale != 1 && (t->desc.target != 1 || t->layout.layers != 1 || t->desc.width < 128 || t->desc.height < 64)) scale = 1;
        entry->scale = scale;
        VkImageAspectFlags aspect = t->desc.format == 0x31 ? VK_IMAGE_ASPECT_STENCIL_BIT : t->desc.format >= 0x32 && t->desc.format <= 0x36 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        if (t->desc.format == 0x35 || t->desc.format == 0x36) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        DkrVkImageInfo info = {format(s, t->desc.format, 1), (uint32_t)ceilf(t->desc.width * scale), (uint32_t)ceilf(t->desc.height * scale),
                               t->desc.levels ? t->desc.levels : 1, t->layout.layers, aspect};
        REQUIRE(dkr_vk_image_create(s->gfx, &info, &entry->image));
        static unsigned logged_images;
        if (logged_images < 16 || getenv("DKR_VERBOSE_NVN")) {
            logged_images++;
            hos_log("replay: image #%u %ux%u format %#x at %#llx\n", t->serial, info.width, info.height, t->desc.format, (unsigned long long)t->address);
        }
    }
    if (sample && !entry->uploaded && !entry->rendered) {
        int64_t t0 = dkr_perf_on ? dkr_perf_now() : 0;
        if (entry->image->info.aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
            entry->uploaded = 1;
        } else {
            for (unsigned layer = 0; layer < t->layout.layers; layer++) for (unsigned level = 0; level < (t->desc.levels ? t->desc.levels : 1); level++) {
                NvnTexLevel l;
                if (nvn_tex_level(&t->desc, level, layer, &l) || l.linear_size > 0x10000000 || l.depth != 1)
                    hos_stop(s->cpu, "NVN replay: texture upload geometry is unsupported");
                uint8_t *bytes = malloc((size_t)l.linear_size);
                if (!bytes) hos_stop(s->cpu, "NVN replay: out of texture upload memory");
                if (nvn_tex_linearize(&t->desc, PTR(t->address), t->layout.size, level, layer, bytes, l.linear_size))
                    hos_stop(s->cpu, "NVN replay: texture storage does not contain its upload region");
                if (t->desc.format >= 0x79 && t->desc.format <= 0x94) {
                    size_t size = (size_t)l.width*l.height*4;
                    uint8_t *rgba = malloc(size);
                    if (!rgba) hos_stop(s->cpu, "NVN replay: out of ASTC decode memory");
                    astc_decode_ldr(bytes, l.width, l.height, l.block_width, l.block_height, t->desc.format >= 0x87, rgba);
                    free(bytes); bytes = rgba; l.linear_size = size;
                }
                REQUIRE(dkr_vk_image_upload(s->gfx, entry->image, level, layer, bytes, (size_t)l.linear_size));
                free(bytes);
            }
            entry->uploaded = 1;
        }
        if (dkr_perf_on) { dkr_perf.upload += dkr_perf_now() - t0; dkr_perf.uploads++; }
    }
    return entry;
}
static DkrVkView *view(Replay *s, Image *entry, const NvnTextureInfo *t, int attachment) {
    DkrVkViewInfo info = {.format = format(s, t->format, 1), .aspects = entry->image->info.aspect,
        .type = VK_IMAGE_VIEW_TYPE_2D, .base_level = t->base_level, .levels = attachment ? 1 : t->levels,
        .base_layer = t->base_layer, .layers = attachment ? 1 : t->layers};
    if (!attachment) {
        if (t->target == 4) info.type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        else if (t->target == 8) info.type = VK_IMAGE_VIEW_TYPE_CUBE;
        else if (t->target != 1 && t->target != 7) hos_stop(s->cpu, "NVN replay: view target %u needs support", t->target);
        if (info.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) info.aspects = t->ds_mode ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        VkComponentSwizzle swizzles[4];
        for (unsigned i = 0; i < 4; i++) {
            if (t->swizzle[i] > 5) hos_stop(s->cpu, "NVN replay: invalid texture component swizzle");
            swizzles[i] = (VkComponentSwizzle)(t->swizzle[i] + 1);
        }
        info.components = (VkComponentMapping){swizzles[0], swizzles[1], swizzles[2], swizzles[3]};
    }
    DkrVkView *out = NULL; REQUIRE(dkr_vk_image_view(s->gfx, entry->image, &info, &out));
    return out;
}
static VkSamplerAddressMode wrap(Replay *s, unsigned v) {
    switch (v) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default: hos_stop(s->cpu, "NVN replay: legacy sampler wrap %u needs emulation", v); return 0;
    }
}
static DkrVkSampler *sampler(Replay *s, uint64_t handle) {
    uint8_t b[0x48]; dkr_nvn_sampler_binding(s->cpu, s->sampler_pool, (unsigned)(handle >> 20) & 0xfff, b);
    Sampler *entry = samplers;
    while (entry && memcmp(entry->builder, b, sizeof b)) entry = entry->next;
    if (entry) return entry->sampler;
    unsigned min = u32(b + 8), mag = u32(b + 12);
    if (min > 5 || mag > 1 || u32(b + 0x44)) hos_stop(s->cpu, "NVN replay: sampler filter/reduction mode needs support");
    VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .minFilter = min & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST, .magFilter = mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = min >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = wrap(s, u32(b + 0x10)), .addressModeV = wrap(s, u32(b + 0x14)), .addressModeW = wrap(s, u32(b + 0x18)),
        .minLod = f32(b + 0x1c), .maxLod = min < 2 ? 0 : f32(b + 0x20), .mipLodBias = f32(b + 0x24),
        .compareEnable = u32(b + 0x28) != 0, .compareOp = (VkCompareOp)(u32(b + 0x2c) - 1),
        .anisotropyEnable = f32(b + 0x40) > 1, .maxAnisotropy = f32(b + 0x40), .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    float color[4]; memcpy(color, b + 0x30, sizeof color);
    if (color[0] == 1 && color[1] == 1 && color[2] == 1 && color[3] == 1) info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if (color[0] == 0 && color[1] == 0 && color[2] == 0 && color[3] == 1) info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if ((color[0] || color[1] || color[2] || color[3]) &&
        (info.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || info.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || info.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER))
        hos_stop(s->cpu, "NVN replay: custom sampler border color requires extension support");
    entry = calloc(1, sizeof *entry);
    if (!entry) hos_stop(s->cpu, "NVN replay: out of sampler cache memory");
    memcpy(entry->builder, b, sizeof b); entry->next = samplers; samplers = entry;
    REQUIRE(dkr_vk_sampler_create(s->gfx, &info, &entry->sampler));
    return entry->sampler;
}

static uint32_t canonical_format_type(uint32_t fmt) {
    switch (fmt) {
    case 0: return 0;
    case 3: case 8: case 0xf: case 0x14: case 0x1b: case 0x27: case 0x2c: case 0x2f: case 0x3e: return 8;
    case 4: case 9: case 0x10: case 0x15: case 0x1c: case 0x28: case 0x2d: case 0x30: return 9;
    default: return 0x25;
    }
}
static uint32_t canonical_primitive(unsigned p) {
    if (p == 0) return 0;
    if (p == 1 || p == 2 || p == 3) return 1;
    if (p == 10 || p == 11) return 10;
    if (p == 12 || p == 13) return 12;
    return 4;
}
static StageInfo *stage_info_for(Replay *s, const char key[33]) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned i = 0; i < 32; i++) h = (h ^ (uint8_t)key[i]) * 1099511628211ULL;
    unsigned bucket = (unsigned)((h ^ (h >> 32)) & 511);
    for (StageInfo *e = stage_info_buckets[bucket]; e; e = e->next) if (!memcmp(e->key, key, 33)) return e;
    StageInfo *e = calloc(1, sizeof *e);
    if (!e) hos_stop(s->cpu, "NVN replay: out of stage-info memory");
    memcpy(e->key, key, 33);
    e->next = stage_info_buckets[bucket];
    stage_info_buckets[bucket] = e;
    return e;
}
static unsigned stage_entry_hash(const char key[33], const NvnShaderInputs *in) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned i = 0; i < 32; i++) h = (h ^ (uint8_t)key[i]) * 1099511628211ULL;
    const uint64_t *words = (const uint64_t *)in;
    for (unsigned i = 0; i < sizeof(*in) / 8; i++) h = (h ^ words[i]) * 1099511628211ULL;
    const uint8_t *tail = (const uint8_t *)in;
    for (unsigned i = (sizeof(*in) / 8) * 8; i < sizeof(*in); i++) h = (h ^ tail[i]) * 1099511628211ULL;
    return (unsigned)((h ^ (h >> 32)) & 1023);
}
static unsigned program_hash(const uint32_t ids[6]) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned i = 0; i < 6; i++) h = (h ^ ids[i]) * 1099511628211ULL;
    return (unsigned)((h ^ (h >> 32)) & 1023);
}

static void shader_inputs(Replay *s, unsigned stage, unsigned primitive, uint32_t used_tex_mask, NvnShaderInputs *out) {
    memset(out, 0, sizeof *out);
    if (stage == 0) {
        for (unsigned i = 0; i < s->attribute_count; i++)
            out->attributes[i] = canonical_format_type((u32(s->attributes[i]) >> 21) & 1023);
        out->transform_feedback = s->xfb_active ? 1 : 0;
    } else if (stage == 1) {
        for (unsigned i = 0; i < s->target_count; i++)
            out->outputs[i] = canonical_format_type(s->colors[i].format);
    } else if (stage == 2) {
        out->primitive = canonical_primitive(primitive);
    }
    if (s->texture_pool && used_tex_mask) {
        uint32_t mask = used_tex_mask;
        while (mask) {
            unsigned i = (unsigned)__builtin_ctz(mask);
            mask &= mask - 1;
            if (s->textures[stage][i]) {
                NvnTextureInfo t;
                dkr_nvn_texture_binding(s->cpu, s->texture_pool, (unsigned)s->textures[stage][i] & 0xfffff, &t);
                out->texture_mask |= 1u << i;
                out->texture_targets[i] = t.target == 7 ? 1 : t.target;
            }
        }
    }
}

static StageEntry *stage_entry(Replay *s, unsigned stage, unsigned primitive, const NvnShader *sh) {
    StageInfo *info = stage_info_for(s, sh->key);
    NvnShaderInputs inputs;
    shader_inputs(s, stage, primitive, info->has_mask ? info->used_tex_mask : 0xffffffffu, &inputs);
    unsigned h = stage_entry_hash(sh->key, &inputs);
    for (StageEntry *e = stage_buckets[h]; e; e = e->hash_next)
        if (!memcmp(e->key, sh->key, 33) && !memcmp(&e->inputs, &inputs, sizeof inputs)) return e;
    StageEntry *e = calloc(1, sizeof *e);
    if (!e) hos_stop(s->cpu, "NVN replay: out of stage-cache memory");
    memcpy(e->key, sh->key, 33);
    nvn_shader_translate(s->cpu, sh, &inputs, &e->cache);
    if (!info->has_mask) {
        uint32_t used = 0;
        for (unsigned i = 0; i < e->cache.binding_count; i++) {
            const DkrShaderBinding *b = &e->cache.bindings[i];
            if (b->kind == DKR_SHADER_TEXTURE || b->kind == DKR_SHADER_IMAGE) {
                unsigned offset = (unsigned)b->handle & 0x3fff;
                if ((b->cb_slot != -1 && b->cb_slot != 2) || (unsigned)b->handle >> 28 || offset < 8 || (offset - 8) % 2 || (offset - 8) / 2 >= 32)
                    used = 0xffffffffu;
                else
                    used |= 1u << ((offset - 8) / 2);
            }
        }
        info->used_tex_mask = used;
        info->has_mask = 1;
        for (unsigned i = 0; i < 32; i++) if (!(used & (1u << i))) {
            inputs.texture_mask &= ~(1u << i);
            inputs.texture_targets[i] = 0;
            inputs.texture_formats[i] = 0;
        }
        h = stage_entry_hash(sh->key, &inputs);
    }
    e->inputs = inputs;
    e->id = ++next_stage_id;
    e->next = stage_entries; stage_entries = e;
    e->hash_next = stage_buckets[h]; stage_buckets[h] = e;
    return e;
}

static Program *program(Replay *s, unsigned primitive, const NvnShader *selected[6]) {
    StageEntry *entries[6] = {0};
    uint32_t stage_ids[6] = {0};
    unsigned count = 0;
    for (unsigned stage = 0; stage < 6; stage++) {
        selected[stage] = NULL;
        if (!s->programs[stage]) continue;
        const NvnProgramSnapshot *snapshot = dkr_nvn_program_snapshot(s->cpu, s->programs[stage]);
        for (unsigned i = 0; i < snapshot->count; i++) if (snapshot->shaders[i].stage == stage) selected[stage] = &snapshot->shaders[i];
        if (!selected[stage]) continue;
        entries[stage] = stage_entry(s, stage, primitive, selected[stage]);
        stage_ids[stage] = entries[stage]->id;
        count++;
    }
    unsigned ph = program_hash(stage_ids);
    for (Program *p = program_buckets[ph]; p; p = p->hash_next)
        if (!memcmp(p->stage_ids, stage_ids, sizeof stage_ids)) return p;
    Program *p = calloc(1, sizeof *p);
    if (!p) hos_stop(s->cpu, "NVN replay: out of program cache memory");
    memcpy(p->stage_ids, stage_ids, sizeof stage_ids);
    p->next = programs; programs = p;
    p->hash_next = program_buckets[ph]; program_buckets[ph] = p;
    DkrVkShaderInfo shaders[6]; DkrVkBinding bindings[128]; unsigned n = 0, nb = 0;
    for (unsigned stage = 0; stage < 6; stage++) if (selected[stage]) {
        p->shaders[stage] = entries[stage]->cache;
        DkrShaderCache *cache = &p->shaders[stage];
        shaders[n++] = (DkrVkShaderInfo){stage, cache->spv, cache->spv_size};
        for (unsigned i = 0; i < cache->binding_count; i++) {
            DkrShaderBinding *b = &cache->bindings[i];
            VkDescriptorType type;
            switch (b->kind) {
            case DKR_SHADER_SUPPORT: case DKR_SHADER_CBUF: type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
            case DKR_SHADER_TEXTURE: type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; break;
            case DKR_SHADER_IMAGE: type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; break;
            case DKR_SHADER_SAMPLER: type = VK_DESCRIPTOR_TYPE_SAMPLER; break;
            default: hos_stop(s->cpu, "NVN replay: shader binding kind %u needs backend support", b->kind); type = 0;
            }
            if (nb == 128) hos_stop(s->cpu, "NVN replay: more than 128 bindings in one program");
            bindings[nb++] = (DkrVkBinding){stage, b->binding, type, b->count};
        }
    }
    if (n != count || !selected[0]) hos_stop(s->cpu, "NVN replay: graphics draw has no vertex shader");
    REQUIRE(dkr_vk_program_create(s->gfx, shaders, n, bindings, nb, &p->program));
    return p;
}

static void support(uint8_t data[1408], float width, float height) {
    /* Ryujinx SupportBuffer (with the pinned point-size patch), as in TotK's support_fill (MIT).
     * Copyright (c) Ryujinx Team and Contributors / 2026 Valiantsin Dzerakh; notices preserve both licenses. */
    memset(data, 0, 1408);
    float inverse[] = {width ? 2.f/width : 0, height ? 2.f/height : 0, 0, 0}, size[] = {width, height, 0, 0}, one = 1;
    memcpy(data + 144, inverse, 16); memcpy(data + 160, size, 16); memcpy(data + 1392, &one, 4);
    for (unsigned i = 0; i < 73; i++) memcpy(data + 192 + 16*i, &one, 4);
}

/* Ryujinx's translator rescales texelFetch coordinates and textureSize results by render_scale[1 + i] for the i-th
 * texture descriptor of the fragment stage (vertex: 1 + fragment count + i), and divides gl_FragCoord by render_scale[0].
 * Descriptors are numbered in binding order; images follow the textures. */
static float texture_scale(Replay *s, unsigned stage, const DkrShaderBinding *b) {
    unsigned offset = (unsigned)b->handle & 0x3fff;
    uint64_t handle = offset >= 8 && (offset - 8) % 2 == 0 && (offset - 8) / 2 < 32 ? s->textures[stage][(offset - 8) / 2] : 0;
    if (!handle || !s->texture_pool) return 1;
    NvnTextureInfo t; dkr_nvn_texture_binding(s->cpu, s->texture_pool, (unsigned)handle & 0xfffff, &t);
    Image *e = find_image(&t);
    return e ? e->scale : 1;
}
static void render_scales(Replay *s, Program *p, const NvnShader *selected[6], float ts, uint8_t data[1408]) {
    float scales[73]; int32_t fragment_count = 0; unsigned n = 1;
    scales[0] = ts;
    const unsigned order[2] = {1, 0}; /* fragment first, then vertex */
    for (unsigned k = 0; k < 2; k++) {
        unsigned stage = order[k];
        if (!selected[stage]) continue;
        DkrShaderCache *cache = &p->shaders[stage];
        for (int kind = 0; kind < 2; kind++) {
            DkrShaderBinding *list[64]; unsigned count = 0;
            for (unsigned i = 0; i < cache->binding_count && count < 64; i++)
                if (cache->bindings[i].kind == (kind ? DKR_SHADER_IMAGE : DKR_SHADER_TEXTURE)) list[count++] = &cache->bindings[i];
            for (unsigned i = 1; i < count; i++) /* insertion sort by binding */
                for (unsigned j = i; j > 0 && list[j - 1]->binding > list[j]->binding; j--) { DkrShaderBinding *t = list[j]; list[j] = list[j - 1]; list[j - 1] = t; }
            for (unsigned i = 0; i < count && n < 73; i++) {
                scales[n++] = texture_scale(s, stage, list[i]);
                if (stage == 1) fragment_count++;
            }
        }
    }
    memcpy(data + 176, &fragment_count, 4);
    for (unsigned i = 0; i < n; i++) memcpy(data + 192 + 16 * i, &scales[i], 4);
}

static VkPrimitiveTopology topology(Replay *s, unsigned primitive) {
    switch (primitive) {
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 10: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; case 11: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case 12: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; case 13: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    default: hos_stop(s->cpu, "NVN replay: primitive %u needs conversion", primitive); return 0;
    }
}

static DkrVkBuffer *guest_buffer(Replay *s, uint64_t address, uint64_t size) {
    for (unsigned i = 0; i < s->uploaded_count; i++)
        if (s->uploaded[i].address == address && s->uploaded[i].size == size) return s->uploaded[i].buffer;
    DkrVkBuffer *buffer = NULL;
    REQUIRE(dkr_vk_buffer_temporary(s->gfx, PTR(address), (size_t)size, &buffer));
    unsigned slot = s->uploaded_count < 256 ? s->uploaded_count++ : s->uploaded_next++ % 256;
    s->uploaded[slot].address = address; s->uploaded[slot].size = size; s->uploaded[slot].buffer = buffer;
    return buffer;
}
static void invalidate_uploaded(Replay *s, uint64_t address, uint64_t size) {
    uint64_t end = address + size;
    unsigned dst = 0;
    for (unsigned i = 0; i < s->uploaded_count; i++) {
        uint64_t a = s->uploaded[i].address, b = a + s->uploaded[i].size;
        if (b <= address || a >= end) s->uploaded[dst++] = s->uploaded[i];
    }
    s->uploaded_count = dst;
    if (s->uploaded_next > dst) s->uploaded_next = 0;
}

static VkRect2D clear_rect(Replay *s, Image *entry, const NvnTextureInfo *t) {
    int64_t width = entry->image->info.width >> t->base_level, height = entry->image->info.height >> t->base_level;
    if (!width) width = 1; if (!height) height = 1;
    float scale = entry->scale;
    int64_t x = s->has_scissor ? (int64_t)floorf(s->scissor[0] * scale) : 0;
    int64_t y = s->has_scissor ? (int64_t)floorf(s->scissor[1] * scale) : 0;
    int64_t right = s->has_scissor ? (int64_t)ceilf(((int64_t)s->scissor[0] + s->scissor[2]) * scale) : width;
    int64_t bottom = s->has_scissor ? (int64_t)ceilf(((int64_t)s->scissor[1] + s->scissor[3]) * scale) : height;
    if (x < 0) x = 0; if (x > width) x = width;
    if (y < 0) y = 0; if (y > height) y = height;
    if (right < x) right = x; if (right > width) right = width;
    if (bottom < y) bottom = y; if (bottom > height) bottom = height;
    return (VkRect2D){{(int32_t)x, (int32_t)y}, {(uint32_t)(right-x), (uint32_t)(bottom-y)}};
}

static void draw(Replay *s, unsigned primitive, unsigned first, unsigned count, unsigned instances, unsigned base_instance,
                  uint64_t indices, unsigned index_type, int32_t base_vertex) {
    if (!count || !instances) return;
    if (s->discard && !s->xfb_active) return;
    if (s->xfb_active) {
        if (!dkr_vk_feedback_supported(s->gfx)) hos_stop(s->cpu, "NVN replay: animated vertices require VK_EXT_transform_feedback");
        if (primitive != 0 || instances != 1) hos_stop(s->cpu, "NVN replay: transform feedback topology/instances require support");
        static unsigned traced;
        if (s->xfb_active && getenv("DKR_TRACE_XFB") && traced++ < 12) {
            hos_log("xfb: capturing primitive %u first %u count %u instances %u\n", primitive, first, count, instances);
            for (unsigned stage = 0; stage < 6; stage++) if (s->programs[stage]) {
                const NvnProgramSnapshot *snapshot = dkr_nvn_program_snapshot(s->cpu, s->programs[stage]);
                for (unsigned i = 0; i < snapshot->count; i++) if (snapshot->shaders[i].stage == stage)
                    hos_log("xfb: stage %u shader %s control %u\n", stage, snapshot->shaders[i].key, snapshot->shaders[i].control_size);
            }
        }
    }
    DkrVkDraw d = {0}; DkrVkDescriptor descriptors[128]; unsigned nd = 0;
    const NvnShader *selected[6];
    int64_t t0 = dkr_perf_on ? dkr_perf_now() : 0;
    Program *p = program(s, primitive, selected);
    if (dkr_perf_on) { dkr_perf.program += dkr_perf_now() - t0; dkr_perf.draws++; }
    d.program = p->program; d.pipeline.topology = topology(s, primitive); d.pipeline.color_count = s->discard ? 0 : s->target_count;
    d.pipeline.rasterizer_discard = s->discard;
    d.pipeline.depth_test = s->depth_test; d.pipeline.depth_write = s->depth_write; d.pipeline.depth_compare = s->depth_compare;
    d.pipeline.stencil_test = s->stencil_test; d.pipeline.depth_clamp = s->depth_clamp; d.pipeline.depth_bias = s->depth_bias;
    d.pipeline.cull = s->cull; d.pipeline.front = s->winding; d.pipeline.polygon = s->polygon;
    d.pipeline.depth_minus_w = ld32(s->device + 0x197c) == 0;
    if (ld32(s->device + 0x1978) == 0) d.pipeline.front = d.pipeline.front == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    d.pipeline.stencil_front = s->front; d.pipeline.stencil_back = s->back;
    d.pipeline.stencil_front.compareMask = s->stencil_value_mask[0]; d.pipeline.stencil_back.compareMask = s->stencil_value_mask[1];
    d.pipeline.stencil_front.writeMask = s->stencil_mask[0]; d.pipeline.stencil_back.writeMask = s->stencil_mask[1];
    d.pipeline.stencil_front.reference = s->stencil_ref[0]; d.pipeline.stencil_back.reference = s->stencil_ref[1];
    Image *targets[8] = {0}; unsigned width = 0, height = 0;
    /* All targets of a draw share one scale: an existing unscaled target keeps new ones unscaled too. */
    float group = g_render_scale;
    for (unsigned i = 0; i < s->target_count; i++) { Image *e = find_image(&s->colors[i]); if (e && e->scale != group) group = e->scale; }
    if (s->has_depth) { Image *e = find_image(&s->depth); if (e && e->scale != group) group = e->scale; }
    for (unsigned i = 0; i < d.pipeline.color_count; i++) {
        targets[i] = target(s, &s->colors[i], group); d.colors[i] = targets[i]->image;
        d.color_views[i] = view(s, targets[i], &s->colors[i], 1);
        d.pipeline.colors[i] = d.color_views[i]->info.format; d.pipeline.blend[i] = s->blend[i];
        d.pipeline.blend[i].blendEnable = s->blend_enable[i]; d.pipeline.blend[i].colorWriteMask = s->write_mask[i];
        if (!i) { width = d.colors[i]->info.width >> s->colors[i].base_level; height = d.colors[i]->info.height >> s->colors[i].base_level;
            if (!width) width = 1; if (!height) height = 1; }
    }
    Image *depth = NULL;
    if (s->has_depth && !s->discard) { depth = target(s, &s->depth, group); d.depth = depth->image;
        d.depth_view = view(s, depth, &s->depth, 1); d.pipeline.depth = d.depth_view->info.format;
        if (!width) { width = d.depth->info.width >> s->depth.base_level; height = d.depth->info.height >> s->depth.base_level;
            if (!width) width = 1; if (!height) height = 1; } }
    /* Rendering covers the intersection of all attachments (the backend checks the scissor against the smallest).
     * Targets can differ in size: a depth buffer that is not eligible for the render scale stays unscaled. */
    for (unsigned i = 1; i < d.pipeline.color_count; i++) {
        unsigned tw = d.colors[i]->info.width >> s->colors[i].base_level, th = d.colors[i]->info.height >> s->colors[i].base_level;
        if (!tw) tw = 1; if (!th) th = 1;
        if (tw < width) width = tw; if (th < height) height = th;
    }
    if (depth && d.pipeline.color_count) {
        unsigned tw = d.depth->info.width >> s->depth.base_level, th = d.depth->info.height >> s->depth.base_level;
        if (!tw) tw = 1; if (!th) th = 1;
        if (tw < width) width = tw; if (th < height) height = th;
    }
    if (s->discard) width = height = 1;
    if (!width || !height) hos_stop(s->cpu, "NVN replay: draw without targets");
    float ts = targets[0] ? targets[0]->scale : depth ? depth->scale : 1;
    int mixed = depth && depth->scale != ts;
    for (unsigned i = 0; i < s->target_count; i++) if (targets[i] && targets[i]->scale != ts) mixed = 1;
    if (mixed) {
        static int once;
        if (!once++) hos_log("replay: a draw mixes render targets of different scales (color %ux%u x%.2f, depth %ux%u x%.2f)\n",
            s->colors[0].desc.width, s->colors[0].desc.height, ts, depth ? s->depth.desc.width : 0, depth ? s->depth.desc.height : 0,
            depth ? depth->scale : 0.f);
    }
    /* Guest viewport and scissor are in unscaled texels; a scaled target needs them multiplied. */
    float x = s->has_viewport ? s->viewport[0] * ts : 0, y = s->has_viewport ? s->viewport[1] * ts : 0;
    float w = s->has_viewport ? s->viewport[2] * ts : (float)width, h = s->has_viewport ? s->viewport[3] * ts : (float)height;
    if (!(w > 0) || !(h != 0)) return; /* an empty viewport rasterizes nothing on the console either */
    /* NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT (Tropical Freeze) is GL's convention: NDC y = -1 and window y = 0 are row 0 of
     * the target, exactly like a Vulkan viewport with positive height. Keeping that memory layout makes viewports,
     * scissors, gl_FragCoord and render targets later sampled as textures match the console; only presentation flips.
     * The upper-left origin (D3D-like, the TotK case) needs the negative-height viewport instead. The front face is
     * swapped above for the lower-left origin, as a positive-height viewport reverses the winding seen by Vulkan. */
    g_lower_left = ld32(s->device + 0x1978) == 0;
    d.viewport = g_lower_left ? (VkViewport){x, y, w, h, s->depth_range[0], s->depth_range[1]}
                              : (VkViewport){x, y+h, w, -h, s->depth_range[0], s->depth_range[1]};
    int64_t sx = s->has_scissor ? (int64_t)(s->scissor[0] * ts) : 0, sy = s->has_scissor ? (int64_t)(s->scissor[1] * ts) : 0;
    int64_t right = s->has_scissor ? (int64_t)ceilf((s->scissor[0] + s->scissor[2]) * ts) : width;
    int64_t bottom = s->has_scissor ? (int64_t)ceilf((s->scissor[1] + s->scissor[3]) * ts) : height;
    if (s->discard) { sx = sy = 0; right = width; bottom = height; }
    if (sx < 0) sx = 0; if (sy < 0) sy = 0; if (right > width) right = width; if (bottom > height) bottom = height;
    if (right <= sx || bottom <= sy) return;
    d.scissor = (VkRect2D){{(int32_t)sx, (int32_t)sy}, {(uint32_t)(right-sx), (uint32_t)(bottom-sy)}};
    memcpy(d.blend_constant, s->blend_color, sizeof d.blend_constant);
    d.depth_bias_slope = s->bias[0]; d.depth_bias_constant = s->bias[1]; d.depth_bias_clamp = s->bias[2];
    unsigned used_streams = 0;
    for (unsigned i = 0; i < s->attribute_count; i++) {
        uint32_t a = u32(s->attributes[i]), fmt = (a >> 21) & 1023, stream = a & 31;
        if (!fmt) continue;
        if (stream >= 16) hos_stop(s->cpu, "NVN replay: vertex stream %u is outside backend limit", stream);
        d.pipeline.attributes[d.pipeline.attribute_count++] = (VkVertexInputAttributeDescription){i, stream, format(s, fmt, 0), (a >> 7) & 0x3fff};
        used_streams |= 1u << stream;
    }
    for (unsigned i = 0; i < 16; i++) if (used_streams & (1u << i)) {
        uint32_t stride = u32(s->streams[i]), divisor = u32(s->streams[i] + 4);
        if (divisor > 1) hos_stop(s->cpu, "NVN replay: vertex divisor > 1 needs extension support");
        d.pipeline.streams[d.pipeline.stream_count++] = (VkVertexInputBindingDescription){i, stride, divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (!s->vertices[i].address || !s->vertices[i].size || s->vertices[i].size > 0x4000000)
            hos_stop(s->cpu, "NVN replay: vertex range %u invalid or needs pool-backed buffer", i);
        d.vertices[i] = guest_buffer(s, s->vertices[i].address, s->vertices[i].size);
    }
    float vw = w / ts, vh = h / ts;
    if (g_render_scale == 1) {
        if (!s->has_support || s->support_w != vw || s->support_h != vh) {
            support(s->last_support, vw, vh);
            REQUIRE(dkr_vk_buffer_temporary(s->gfx, s->last_support, sizeof s->last_support, &s->support_buffer));
            s->support_w = vw; s->support_h = vh; s->has_support = 1;
        }
    } else {
        uint8_t support_data[1408];
        support(support_data, vw, vh);
        render_scales(s, p, selected, ts, support_data);
        if (!s->has_support || memcmp(s->last_support, support_data, sizeof support_data) != 0) {
            REQUIRE(dkr_vk_buffer_temporary(s->gfx, support_data, sizeof support_data, &s->support_buffer));
            memcpy(s->last_support, support_data, sizeof support_data);
            s->has_support = 1;
        }
    }
    for (unsigned stage = 0; stage < 6; stage++) if (selected[stage]) {
        DkrShaderCache *cache = &p->shaders[stage];
        for (unsigned i = 0; i < cache->binding_count; i++) {
            DkrShaderBinding *b = &cache->bindings[i];
            if (nd == 128) hos_stop(s->cpu, "NVN replay: descriptor list overflow");
            DkrVkDescriptor *out = &descriptors[nd++]; memset(out, 0, sizeof *out); out->stage = stage; out->binding = b->binding;
            if (b->kind == DKR_SHADER_SUPPORT) {
                if (cache->support_size != sizeof s->last_support) hos_stop(s->cpu, "NVN replay: support-buffer ABI changed");
                out->buffer = s->support_buffer; out->size = sizeof s->last_support;
            } else if (b->kind == DKR_SHADER_CBUF) {
                const void *data; size_t size;
                if (b->slot >= 3 && b->slot < 19) {
                    Range *r = &s->uniforms[stage][b->slot - 3];
                    data = PTR(r->address); size = (size_t)r->size;
                    if (!data || !size || size > 65536) hos_stop(s->cpu, "NVN replay: missing/oversized constant buffer stage %u slot %d", stage, b->slot);
                    out->buffer = guest_buffer(s, r->address, r->size); out->size = size;
                } else if (b->slot == 1) {
                    data = selected[stage]->code + selected[stage]->c1_offset;
                    size = selected[stage]->code_size - selected[stage]->c1_offset;
                    if (!data || !size || size > 65536) hos_stop(s->cpu, "NVN replay: missing/oversized constant buffer stage %u slot %d", stage, b->slot);
                    out->buffer = guest_buffer(s, (uint64_t)(uintptr_t)data, size); out->size = size;
                } else if (b->slot == 0 || b->slot == 2) {
                    uint8_t driver[65536] = {0};
                    if (!b->slot) memcpy(driver + 0x644, &base_instance, 4);
                    else for (unsigned t = 0; t < 32; t++) { uint32_t handle = (uint32_t)s->textures[stage][t]; memcpy(driver + 0x20 + 8*t, &handle, 4); }
                    REQUIRE(dkr_vk_buffer_temporary(s->gfx, driver, sizeof driver, &out->buffer)); out->size = sizeof driver;
                } else { hos_stop(s->cpu, "NVN replay: unknown constant-buffer slot %d", b->slot); }
            } else if (b->kind == DKR_SHADER_TEXTURE || b->kind == DKR_SHADER_IMAGE || b->kind == DKR_SHADER_SAMPLER) {
                unsigned offset = (unsigned)b->handle & 0x3fff;
                if ((b->cb_slot != -1 && b->cb_slot != 2) || (unsigned)b->handle >> 28 || offset < 8 || (offset - 8) % 2 || (offset - 8)/2 >= 32)
                    hos_stop(s->cpu, "NVN replay: shader texture handle %#x needs dynamic/separate resolution", b->handle);
                uint64_t handle = s->textures[stage][(offset-8)/2];
                if (!handle) hos_stop(s->cpu, "NVN replay: shader uses an unbound texture");
                if (b->kind != DKR_SHADER_SAMPLER) {
                    NvnTextureInfo t; dkr_nvn_texture_binding(s->cpu, s->texture_pool, (unsigned)handle & 0xfffff, &t);
                    Image *entry = image(s, &t, 1); out->image = entry->image; out->view = view(s, entry, &t, 0);
                }
                if (b->kind != DKR_SHADER_IMAGE) out->sampler = sampler(s, handle);
            } else hos_stop(s->cpu, "NVN replay: binding kind %u needs implementation", b->kind);
        }
    }
    d.descriptors = descriptors; d.descriptor_count = nd;
    d.count = count; d.first = first; d.instances = instances; d.first_instance = base_instance; d.base_vertex = base_vertex;
    if (indices) {
        if (index_type != 1 && index_type != 2) hos_stop(s->cpu, "NVN replay: byte indices need conversion");
        d.index_type = index_type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        d.indices = guest_buffer(s, indices, (uint64_t)count*(index_type == 1 ? 2 : 4));
    }
    NvnXfbLayout feedback_layout;
    if (s->xfb_active) {
        if (selected[2] || selected[3] || selected[4] || nvn_shader_xfb_layout(selected[0], &feedback_layout))
            hos_stop(s->cpu, "NVN replay: transform feedback shader layout requires support");
        for (unsigned i = 0; i < 4; i++) if (feedback_layout.counts[i]) {
            Range range = s->feedback[i]; uint64_t bytes = (uint64_t)count * feedback_layout.strides[i];
            if (!range.address || !range.size || range.size > 0x4000000 || s->feedback_written[i] > range.size ||
                bytes > range.size - s->feedback_written[i]) hos_stop(s->cpu, "NVN replay: invalid transform feedback range");
            FeedbackBuffer *buffer = feedback_buffers;
            while (buffer && (buffer->range.address != range.address || buffer->range.size != range.size)) buffer = buffer->next;
            if (!buffer) {
                buffer = calloc(1, sizeof *buffer);
                if (!buffer) hos_stop(s->cpu, "NVN replay: out of feedback-buffer memory");
                buffer->range = range; buffer->next = feedback_buffers; feedback_buffers = buffer;
                REQUIRE(dkr_vk_buffer_create(s->gfx, NULL, (size_t)range.size, &buffer->buffer));
            }
            d.feedback[i] = buffer->buffer; d.feedback_offsets[i] = s->feedback_written[i]; d.feedback_sizes[i] = bytes;
        }
    }
    REQUIRE(dkr_vk_draw(s->gfx, &d));
    if (s->xfb_active) {
        /* Captured positions/normals must be visible to later draws AND to the guest's CPU-side consumers.
         * A synchronous split preserves ordering while keeping the current batch upload arena intact. */
        REQUIRE(dkr_vk_batch_end(s->gfx));
        for (unsigned i = 0; i < 4; i++) if (d.feedback[i]) {
            uint64_t dst_addr = s->feedback[i].address + d.feedback_offsets[i];
            REQUIRE(dkr_vk_buffer_read(s->gfx, d.feedback[i], (size_t)d.feedback_offsets[i],
                PTR(dst_addr), (size_t)d.feedback_sizes[i]));
            invalidate_uploaded(s, dst_addr, d.feedback_sizes[i]);
            s->feedback_written[i] += d.feedback_sizes[i];
        }
        REQUIRE(dkr_vk_batch_resume(s->gfx));
        if (!g_xfb_skipped++) hos_log("replay: transform feedback vertices captured and synchronized with guest memory\n");
    }
    for (unsigned i = 0; i < s->target_count; i++) if (targets[i]) targets[i]->rendered = 1;
    if (depth) depth->rendered = 1;
    if (++s->draws == 1) {
        hos_log("replay: first NVN draw recorded on Vulkan (%u vertices/indices)\n", count);
        hos_log("replay: window origin %s, depth mode %s\n", ld32(s->device + 0x1978) ? "upper-left" : "lower-left",
                ld32(s->device + 0x197c) ? "near is zero" : "near is -w");
    }
    if (g_trace_draws) {
        hos_log("draw: prim %u count %u inst %u | target %ux%u fmt %#x x%u depth %s | vp %s %.0f,%.0f %.0fx%.0f | sc %s %d,%d %ux%u -> %d,%d %ux%u"
                " | blend %u/%u depth %u/%u stencil %u cull %u | %u descriptors\n",
                primitive, count, instances, width, height, s->target_count ? s->colors[0].format : 0, s->target_count,
                s->has_depth ? "yes" : "no", s->has_viewport ? "set" : "full", x, y, w, h, s->has_scissor ? "set" : "full",
                s->has_scissor ? s->scissor[0] : 0, s->has_scissor ? s->scissor[1] : 0,
                s->has_scissor ? (unsigned)s->scissor[2] : width, s->has_scissor ? (unsigned)s->scissor[3] : height,
                d.scissor.offset.x, d.scissor.offset.y, d.scissor.extent.width, d.scissor.extent.height,
                s->blend_enable[0], s->write_mask[0], s->depth_test, s->depth_write, s->stencil_test, s->cull, nd);
    }
}

static VkCompareOp compare(Replay *s, uint64_t v) {
    if (v < 1 || v > 8) hos_stop(s->cpu, "NVN replay: invalid compare function %llu", (unsigned long long)v);
    return (VkCompareOp)(v - 1);
}
static VkBlendFactor factor(Replay *s, unsigned v) {
    static const VkBlendFactor common[] = {0, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    if (v && v < sizeof common / sizeof *common) return common[v];
    if (v >= 0x10 && v <= 0x13) return (VkBlendFactor)(VK_BLEND_FACTOR_SRC1_COLOR + v - 0x10);
    if (v >= 0x61 && v <= 0x64) return (VkBlendFactor)(VK_BLEND_FACTOR_CONSTANT_COLOR + v - 0x61);
    hos_stop(s->cpu, "NVN replay: unknown blend factor %#x", v); return 0;
}
#define ADDRESS(p) ((uint64_t)(uintptr_t)(p))
#define SCALAR(name, object, arg) get(s, "nvn" name, PTR(object), arg, 0, 0, 0, 0)
static void visit(const NvnRecord *r, void *user) {
    Replay *s = user;
    const char *name = r->name + strlen("nvnCommandBuffer");
    const uint64_t *x = r->x;
#define IS(n) (!strcmp(name, (n)))
    if (IS("SetTexturePool")) s->texture_pool = x[1];
    else if (IS("SetSamplerPool")) s->sampler_pool = x[1];
    else if (IS("SetShaderScratchMemory")) { /* SPIR-V local/private storage replaces Maxwell scratch. */ }
    else if (IS("SetRenderTargets")) {
        if (x[1] > 8) hos_stop(s->cpu, "NVN replay: more than eight render targets");
        s->target_count = (unsigned)x[1];
        for (unsigned i = 0; i < s->target_count; i++) {
            uint64_t texture = ld64(x[2] + 8*i), view = x[3] ? ld64(x[3] + 8*i) : 0;
            if (!texture) hos_stop(s->cpu, "NVN replay: sparse render target needs support");
            dkr_nvn_texture_info(s->cpu, texture, PTR(view), &s->colors[i]);
        }
        s->has_depth = x[4] != 0;
        if (s->has_depth) dkr_nvn_texture_info(s->cpu, x[4], PTR(x[5]), &s->depth);
    } else if (IS("SetViewport")) {
        for (unsigned i = 0; i < 4; i++) s->viewport[i] = (float)(int32_t)x[i+1];
        s->has_viewport = 1;
    } else if (IS("SetScissor")) {
        for (unsigned i = 0; i < 4; i++) s->scissor[i] = (int32_t)x[i+1];
        s->has_scissor = 1;
    } else if (IS("SetDepthRange")) {
        s->depth_range[0] = f32(&r->v[0]); s->depth_range[1] = f32(&r->v[1]);
    } else if (IS("SetViewports") || IS("SetScissors") || IS("SetDepthRanges")) {
        if (x[1] != 0 || x[2] > 1) hos_stop(s->cpu, "NVN replay: multiple viewports/scissors need dynamic support");
        if (!x[2]) return;
        if (IS("SetViewports")) { memcpy(s->viewport, PTR(x[3]), 16); s->has_viewport = 1; }
        else if (IS("SetScissors")) { memcpy(s->scissor, PTR(x[3]), 16); s->has_scissor = 1; }
        else memcpy(s->depth_range, PTR(x[3]), 8);
    } else if (IS("BindProgram")) {
        if (x[2] & ~63ull) hos_stop(s->cpu, "NVN replay: invalid shader-stage mask");
        for (unsigned i = 0; i < 6; i++) if (x[2] & (1u << i)) s->programs[i] = x[1];
    } else if (IS("BindVertexAttribState") || IS("BindVertexStreamState")) {
        if (x[1] > 16) hos_stop(s->cpu, "NVN replay: too many vertex states");
        if (IS("BindVertexAttribState")) { memset(s->attributes, 0, sizeof s->attributes); memcpy(s->attributes, PTR(x[2]), (size_t)x[1]*4); s->attribute_count = (unsigned)x[1]; }
        else { memset(s->streams, 0, sizeof s->streams); memcpy(s->streams, PTR(x[2]), (size_t)x[1]*8); s->stream_count = (unsigned)x[1]; }
    } else if (IS("BindVertexBuffers")) {
        if (x[1] > 16 || x[2] > 16 - x[1]) hos_stop(s->cpu, "NVN replay: invalid vertex range array");
        memcpy(s->vertices + x[1], PTR(x[3]), (size_t)x[2] * sizeof(Range));
    } else if (IS("BindVertexBuffer")) {
        if (x[1] >= 16) hos_stop(s->cpu, "NVN replay: invalid vertex slot");
        s->vertices[x[1]] = (Range){x[2], x[3]};
    } else if (IS("BindUniformBuffer")) {
        if (x[1] >= 6 || x[2] >= 16) hos_stop(s->cpu, "NVN replay: invalid uniform stage/slot");
        s->uniforms[x[1]][x[2]] = (Range){x[3], x[4]};
    } else if (IS("BindUniformBuffers")) {
        if (x[1] >= 6 || x[2] > 16 || x[3] > 16 - x[2]) hos_stop(s->cpu, "NVN replay: invalid uniform range array");
        memcpy(s->uniforms[x[1]] + x[2], PTR(x[4]), (size_t)x[3] * sizeof(Range));
    } else if (IS("BindTexture")) {
        if (x[1] >= 6 || x[2] >= 32) hos_stop(s->cpu, "NVN replay: invalid texture slot");
        s->textures[x[1]][x[2]] = x[3];
    } else if (IS("BindTextures")) {
        if (x[1] >= 6 || x[2] > 32 || x[3] > 32 - x[2]) hos_stop(s->cpu, "NVN replay: invalid texture array");
        memcpy(s->textures[x[1]] + x[2], PTR(x[4]), (size_t)x[3] * 8);
    } else if (IS("BindBlendState")) {
        unsigned target = (unsigned)SCALAR("BlendStateGetBlendTarget", x[1], 0), f[4], equation[2];
        if (target >= 8) hos_stop(s->cpu, "NVN replay: invalid blend target");
        if (SCALAR("BlendStateGetAdvancedMode", x[1], 0)) hos_stop(s->cpu, "NVN replay: advanced blend requires support");
        get(s, "nvnBlendStateGetBlendFunc", PTR(x[1]), ADDRESS(&f[0]), ADDRESS(&f[1]), ADDRESS(&f[2]), ADDRESS(&f[3]), 0);
        get(s, "nvnBlendStateGetBlendEquation", PTR(x[1]), ADDRESS(&equation[0]), ADDRESS(&equation[1]), 0, 0, 0);
        if (!equation[0] || equation[0] > 5 || !equation[1] || equation[1] > 5) hos_stop(s->cpu, "NVN replay: invalid blend equation");
        s->blend[target] = (VkPipelineColorBlendAttachmentState){.srcColorBlendFactor = factor(s, f[0]), .dstColorBlendFactor = factor(s, f[1]),
            .srcAlphaBlendFactor = factor(s, f[2]), .dstAlphaBlendFactor = factor(s, f[3]),
            .colorBlendOp = (VkBlendOp)(equation[0]-1), .alphaBlendOp = (VkBlendOp)(equation[1]-1)};
    } else if (IS("BindColorState")) {
        if (SCALAR("ColorStateGetLogicOp", x[1], 0) != 3) hos_stop(s->cpu, "NVN replay: logic operation needs pipeline support");
        if (SCALAR("ColorStateGetAlphaTest", x[1], 0) != 8) hos_stop(s->cpu, "NVN replay: alpha-test variant needs shader support");
        for (unsigned i = 0; i < 8; i++) s->blend_enable[i] = (uint32_t)SCALAR("ColorStateGetBlendEnable", x[1], i);
    } else if (IS("BindChannelMaskState")) {
        for (unsigned i = 0; i < 8; i++) {
            uint32_t color[4];
            get(s, "nvnChannelMaskStateGetChannelMask", PTR(x[1]), i, ADDRESS(&color[0]), ADDRESS(&color[1]), ADDRESS(&color[2]), ADDRESS(&color[3]));
            s->write_mask[i] = (color[0] ? 1 : 0) | (color[1] ? 2 : 0) | (color[2] ? 4 : 0) | (color[3] ? 8 : 0);
        }
    } else if (IS("BindPolygonState")) {
        unsigned winding = (unsigned)SCALAR("PolygonStateGetFrontFace", x[1], 0), mode = (unsigned)SCALAR("PolygonStateGetPolygonMode", x[1], 0);
        s->cull = (VkCullModeFlags)SCALAR("PolygonStateGetCullFace", x[1], 0);
        s->winding = winding ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        if (mode > 2) hos_stop(s->cpu, "NVN replay: invalid polygon mode");
        s->polygon = mode == 2 ? VK_POLYGON_MODE_FILL : mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_POINT;
        unsigned offsets = (unsigned)SCALAR("PolygonStateGetPolygonOffsetEnables", x[1], 0);
        s->depth_bias = !!(offsets & (1u << mode));
    } else if (IS("BindDepthStencilState")) {
        s->depth_test = (VkBool32)SCALAR("DepthStencilStateGetDepthTestEnable", x[1], 0);
        s->depth_write = (VkBool32)SCALAR("DepthStencilStateGetDepthWriteEnable", x[1], 0);
        s->depth_compare = compare(s, SCALAR("DepthStencilStateGetDepthFunc", x[1], 0));
        s->stencil_test = (VkBool32)SCALAR("DepthStencilStateGetStencilTestEnable", x[1], 0);
        if (s->stencil_test) for (unsigned i = 0; i < 2; i++) {
            uint32_t ops[3]; VkStencilOpState *stencil = i ? &s->back : &s->front;
            stencil->compareOp = compare(s, SCALAR("DepthStencilStateGetStencilFunc", x[1], i + 1));
            get(s, "nvnDepthStencilStateGetStencilOp", PTR(x[1]), i + 1, ADDRESS(&ops[0]), ADDRESS(&ops[1]), ADDRESS(&ops[2]), 0);
            for (unsigned k = 0; k < 3; k++) if (ops[k] < 1 || ops[k] > 8) hos_stop(s->cpu, "NVN replay: invalid stencil op");
            stencil->failOp = (VkStencilOp)(ops[0]-1); stencil->depthFailOp = (VkStencilOp)(ops[1]-1); stencil->passOp = (VkStencilOp)(ops[2]-1);
        }
    } else if (IS("BindMultisampleState")) {
        if (SCALAR("MultisampleStateGetSamples", x[1], 0) > 1) hos_stop(s->cpu, "NVN replay: multisample state requires support");
    } else if (IS("SetStencilValueMask") || IS("SetStencilMask") || IS("SetStencilRef")) {
        for (unsigned i = 0; i < 2; i++) if (x[1] & (1u << i)) {
            if (IS("SetStencilValueMask")) s->stencil_value_mask[i] = (uint32_t)x[2];
            else if (IS("SetStencilMask")) s->stencil_mask[i] = (uint32_t)x[2]; else s->stencil_ref[i] = (uint32_t)x[2];
        }
    } else if (IS("SetBlendColor")) memcpy(s->blend_color, PTR(x[1]), 16);
    else if (IS("SetDepthClamp")) s->depth_clamp = x[1] != 0;
    else if (IS("SetPolygonOffsetClamp")) for (unsigned i = 0; i < 3; i++) s->bias[i] = f32(&r->v[i]);
    else if (IS("SetConservativeRasterEnable")) { if (x[1]) hos_stop(s->cpu, "NVN replay: conservative rasterization needs support"); }
    else if (IS("BindTransformFeedbackBuffer")) {
        if (x[1] >= 4) hos_stop(s->cpu, "NVN replay: invalid transform feedback slot");
        s->feedback[x[1]] = (Range){x[2], x[3]};
    } else if (IS("BindTransformFeedbackBuffers")) {
        if (x[1] > 4 || x[2] > 4-x[1]) hos_stop(s->cpu, "NVN replay: invalid transform feedback slots");
        memcpy(s->feedback+x[1], PTR(x[3]), (size_t)x[2]*sizeof(Range));
    } else if (IS("BeginTransformFeedback")) { s->xfb_active = 1; memset(s->feedback_written, 0, sizeof s->feedback_written); }
    else if (IS("ResumeTransformFeedback")) s->xfb_active = 1;
    else if (IS("EndTransformFeedback") || IS("PauseTransformFeedback")) s->xfb_active = 0;
    else if (IS("SetRasterizerDiscard")) s->discard = x[1] != 0;
    else if (IS("DrawTransformFeedback")) {
        hos_stop(s->cpu, "NVN replay: indirect transform feedback draw requires counter support");
    }
    else if (IS("Barrier")) { /* Full resource barriers surround each draw/upload in vk_draw.c. */ }
    else if (IS("UpdateUniformBuffer")) {
        if (x[3] > x[2] || x[4] > x[2] - x[3]) hos_stop(s->cpu, "NVN replay: uniform update exceeds its range");
        memcpy(PTR(x[1] + x[3]), PTR(x[5]), (size_t)x[4]);
        invalidate_uploaded(s, x[1] + x[3], x[4]); /* only drop cached slices overlapping the updated range */
    } else if (IS("CopyBufferToTexture")) {
        NvnTextureInfo t; dkr_nvn_texture_info(s->cpu, x[2], PTR(x[3]), &t);
        uint32_t box[6]; memcpy(box, PTR(x[4]), sizeof box);
        if (x[5] || box[2] || box[5] != 1 || r->copy_row_stride < 0)
            hos_stop(s->cpu, "NVN replay: copy flags/depth/negative stride need support");
        Image *destination = image(s, &t, 0);
        NvnTexLevel level;
        if (nvn_tex_level(&t.desc, t.base_level, t.base_layer, &level)) hos_stop(s->cpu, "NVN replay: copy mip is unsupported");
        if (!box[3] || !box[4] || box[0] > level.width || box[3] > level.width - box[0] || box[1] > level.height || box[4] > level.height - box[1])
            hos_stop(s->cpu, "NVN replay: copy rectangle exceeds its mip");
        unsigned bpb = (unsigned)nvn_tex_bytes_per_block((int)t.desc.format);
        size_t row = (size_t)((box[3] + level.block_width - 1)/level.block_width)*bpb;
        unsigned rows = (box[4] + level.block_height - 1)/level.block_height;
        size_t stride = r->copy_row_stride ? (size_t)r->copy_row_stride : row;
        if (!row || !rows || stride < row || row*rows > 0x10000000) hos_stop(s->cpu, "NVN replay: invalid copy extent/stride");
        size_t size = row*rows;
        int is_astc = t.desc.format >= 0x79 && t.desc.format <= 0x94;
        const uint8_t *bytes = NULL;
        uint8_t *owned = NULL;
        if (stride == row && !is_astc) {
            bytes = PTR(x[1]);
        } else {
            owned = malloc(size);
            if (!owned) hos_stop(s->cpu, "NVN replay: out of copy staging memory");
            for (unsigned y = 0; y < rows; y++) memcpy(owned + y*row, PTR(x[1] + y*stride), row);
            if (is_astc) {
                size = (size_t)box[3]*box[4]*4;
                uint8_t *rgba = malloc(size);
                if (!rgba) hos_stop(s->cpu, "NVN replay: out of ASTC copy memory");
                astc_decode_ldr(owned, box[3], box[4], level.block_width, level.block_height, t.desc.format >= 0x87, rgba);
                free(owned); owned = rgba;
            }
            bytes = owned;
        }
        if (destination->scale == 1)
            REQUIRE(dkr_vk_image_upload_region(s->gfx, destination->image, t.base_level, t.base_layer, box[0], box[1], box[3], box[4], bytes, size));
        else { /* a scaled render target: upload into an unscaled stand-in of the same format, then blit it up */
            Staging *st = stagings;
            while (st && (st->image->info.format != destination->image->info.format || st->image->info.width < box[3] || st->image->info.height < box[4])) st = st->next;
            if (!st) {
                st = calloc(1, sizeof *st);
                if (!st) hos_stop(s->cpu, "NVN replay: out of staging-image memory");
                DkrVkImageInfo info = {destination->image->info.format, box[3] < 256 ? 256 : box[3], box[4] < 256 ? 256 : box[4], 1, 1, VK_IMAGE_ASPECT_COLOR_BIT};
                REQUIRE(dkr_vk_image_create(s->gfx, &info, &st->image));
                st->next = stagings; stagings = st;
            }
            REQUIRE(dkr_vk_image_upload_region(s->gfx, st->image, 0, 0, 0, 0, box[3], box[4], bytes, size));
            float k = destination->scale;
            int32_t a[4] = {0, 0, (int32_t)box[3], (int32_t)box[4]};
            int32_t b[4] = {(int32_t)(box[0] * k), (int32_t)(box[1] * k), (int32_t)ceilf((box[0] + box[3]) * k), (int32_t)ceilf((box[1] + box[4]) * k)};
            REQUIRE(dkr_vk_image_blit(s->gfx, st->image, 0, 0, a, destination->image, t.base_level, t.base_layer, b));
        }
        free(owned); destination->rendered = 1;
        static unsigned logged_b2t;
        if (logged_b2t < 16 || getenv("DKR_VERBOSE_NVN")) {
            logged_b2t++;
            hos_log("replay: buffer-to-texture #%u at %u,%u size %ux%u\n", t.serial, box[0], box[1], box[3], box[4]);
        }
    } else if (IS("CopyTextureToTexture")) {
        NvnTextureInfo source, destination;
        dkr_nvn_texture_info(s->cpu, x[1], PTR(x[2]), &source); dkr_nvn_texture_info(s->cpu, x[4], PTR(x[5]), &destination);
        uint32_t from[6], to[6]; memcpy(from, PTR(x[3]), sizeof from); memcpy(to, PTR(x[6]), sizeof to);
        if (x[7] || from[2] || to[2] || from[5] != 1 || to[5] != 1 || from[3] != to[3] || from[4] != to[4])
            hos_stop(s->cpu, "NVN replay: scaled/flipped/layered texture copy needs support");
        Image *src = image(s, &source, 1), *dst = image(s, &destination, 0);
        if (src->scale == 1 && dst->scale == 1)
            REQUIRE(dkr_vk_image_copy(s->gfx, src->image, source.base_level, source.base_layer, from[0], from[1],
                dst->image, destination.base_level, destination.base_layer, to[0], to[1], from[3], from[4]));
        else {
            int32_t a[4] = {(int32_t)(from[0] * src->scale), (int32_t)(from[1] * src->scale),
                            (int32_t)ceilf((from[0] + from[3]) * src->scale), (int32_t)ceilf((from[1] + from[4]) * src->scale)};
            int32_t b[4] = {(int32_t)(to[0] * dst->scale), (int32_t)(to[1] * dst->scale),
                            (int32_t)ceilf((to[0] + to[3]) * dst->scale), (int32_t)ceilf((to[1] + to[4]) * dst->scale)};
            REQUIRE(dkr_vk_image_blit(s->gfx, src->image, source.base_level, source.base_layer, a,
                dst->image, destination.base_level, destination.base_layer, b));
        }
        dst->rendered = 1;
    } else if (IS("DrawArrays")) draw(s, (unsigned)x[1], (unsigned)x[2], (unsigned)x[3], 1, 0, 0, 0, 0);
    else if (IS("DrawArraysInstanced")) draw(s, (unsigned)x[1], (unsigned)x[2], (unsigned)x[3], (unsigned)x[5], (unsigned)x[4], 0, 0, 0);
    else if (IS("DrawElements")) draw(s, (unsigned)x[1], 0, (unsigned)x[3], 1, 0, x[4], (unsigned)x[2], 0);
    else if (IS("DrawElementsInstanced")) draw(s, (unsigned)x[1], 0, (unsigned)x[3], (unsigned)x[7], (unsigned)x[6], x[4], (unsigned)x[2], (int32_t)x[5]);
    else if (IS("ClearDepthStencil")) {
        if (!s->has_depth) return;
        NvnTextureInfo *t = &s->depth;
        VkImageAspectFlags aspects = x[1] ? VK_IMAGE_ASPECT_DEPTH_BIT : 0;
        if ((t->desc.format == 0x35 || t->desc.format == 0x36) && (x[3] & 255)) {
            if ((x[3] & 255) != 255) hos_stop(s->cpu, "NVN replay: masked stencil clear needs support");
            aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        if (!aspects) return;
        Image *e0 = find_image(t);
        Image *entry = target(s, t, e0 ? e0->scale : g_render_scale);
        float depth = f32(&r->v[0]); if (depth < 0) depth = 0; if (depth > 1) depth = 1;
        VkClearValue value = {.depthStencil = {depth, (uint32_t)x[2]}};
        REQUIRE(dkr_vk_attachment_clear(s->gfx, entry->image, view(s, entry, t, 1), clear_rect(s, entry, t), aspects, &value));
        entry->rendered = 1;
    } else if (IS("ClearColor")) {
        if (x[1] >= s->target_count || x[3] != 15) hos_stop(s->cpu, "NVN replay: partial color clear needs attachment support");
        NvnTextureInfo *t = &s->colors[x[1]];
        Image *e0 = find_image(t);
        Image *entry = target(s, t, e0 ? e0->scale : g_render_scale);
        VkClearValue value; memcpy(value.color.float32, PTR(x[2]), 16);
        REQUIRE(dkr_vk_attachment_clear(s->gfx, entry->image, view(s, entry, t, 1), clear_rect(s, entry, t), VK_IMAGE_ASPECT_COLOR_BIT, &value));
        entry->rendered = 1;
    } else hos_stop(s->cpu, "NVN replay: %s is not implemented", r->name);
#undef IS
}

void dkr_nvn_replay_submit(Cpu *cpu, uint64_t device, void **state, const uint64_t *handles, int count) {
    const char *enabled = getenv("DKR_GFX_REPLAY");
    if (enabled && !strcmp(enabled, "0")) return;
    AcquireSRWLockExclusive(&lock);
    Replay *s = *state;
    if (!s) {
        s = calloc(1, sizeof *s); if (!s) hos_stop(cpu, "NVN replay: out of queue-state memory");
        *state = s; s->depth_range[1] = 1; s->depth_compare = VK_COMPARE_OP_ALWAYS;
        for (unsigned i = 0; i < 8; i++) s->write_mask[i] = 15;
        for (unsigned i = 0; i < 2; i++) s->stencil_mask[i] = s->stencil_value_mask[i] = 255;
    }
    s->cpu = cpu; s->gfx = dkr_nvn_graphics(); s->device = device;
    static int scale_read;
    if (!scale_read) {
        scale_read = 1;
        g_render_scale = dkr_settings()->render_scale;
        if (g_render_scale != 1) hos_log("replay: render scale %.2f (render targets of at least 128x64)\n", g_render_scale);
    }
    if (!s->gfx) hos_stop(cpu, "NVN replay: no graphics context");
    int64_t t0 = dkr_perf_on ? dkr_perf_now() : 0;
    REQUIRE(dkr_vk_batch_begin(s->gfx));
    s->uploaded_count = 0; s->has_support = 0; /* arena slices of the previous batch were recycled by batch_begin */
    dkr_nvn_visit_commands(cpu, handles, count, visit, s);
    REQUIRE(dkr_vk_batch_end(s->gfx));
    if (dkr_perf_on) { dkr_perf.replay += dkr_perf_now() - t0; dkr_perf.submits++; }
    ReleaseSRWLockExclusive(&lock);
}
void dkr_nvn_replay_present(Cpu *cpu, uint64_t texture, uint64_t frame) {
    const char *enabled = getenv("DKR_GFX_REPLAY");
    if (enabled && !strcmp(enabled, "0")) return;
    AcquireSRWLockExclusive(&lock);
    int64_t t0 = dkr_perf_on ? dkr_perf_now() : 0;
    Replay state = {.cpu = cpu, .gfx = dkr_nvn_graphics()}, *s = &state;
    NvnTextureInfo info; dkr_nvn_texture_info(cpu, texture, NULL, &info);
    Image *entry = find_image(&info);
    if (!entry || !entry->rendered) hos_stop(cpu, "NVN replay: presented texture has not been rendered");
    if (!dkr_gfx_poll(s->gfx)) hos_stop(cpu, "graphics window closed");
    const char *path = getenv("DKR_GFX_CAPTURE"), *at = getenv("DKR_GFX_CAPTURE_FRAME");
    uint64_t capture_frame = at ? strtoull(at, NULL, 10) : 60;
    /* DKR_GFX_CAPTURE_EVERY=N also saves every Nth frame as <path>.<frame>.bmp. */
    const char *every_text = getenv("DKR_GFX_CAPTURE_EVERY");
    uint64_t every = every_text ? strtoull(every_text, NULL, 10) : 0;
    char numbered[4096];
    const char *capture = NULL;
    if (path && frame == capture_frame) capture = path;
    else if (path && every && frame % every == 0) { /* <path>.<frame>.<ms since the first frame>.bmp */
        static LARGE_INTEGER first; LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
        if (!first.QuadPart) first = now;
        if (snprintf(numbered, sizeof numbered, "%s.%llu.%llu.bmp", path, (unsigned long long)frame,
                     (unsigned long long)((now.QuadPart - first.QuadPart) * 1000 / freq.QuadPart)) < (int)sizeof numbered) capture = numbered;
    }
    uint64_t before = dkr_gfx_frames(s->gfx);
    dkr_gfx_set_present_flip(s->gfx, g_lower_left);
    do {
        REQUIRE(dkr_vk_image_present(s->gfx, entry->image, capture));
        if (dkr_gfx_frames(s->gfx) == before) {
            if (!dkr_gfx_poll(s->gfx)) hos_stop(cpu, "graphics window closed");
            Sleep(1);
        }
    } while (dkr_gfx_frames(s->gfx) == before);
    if (frame == 1) hos_log("replay: first NVN texture presented through Vulkan\n");
    /* New pipelines reach the disk cache once rendering has been quiet for a few seconds. */
    if (dkr_vk_pipeline_cache_save(s->gfx, 1)) {
        static int warned;
        if (!warned++) hos_log("replay: cannot save the pipeline cache: %s\n", dkr_gfx_error(s->gfx));
    }
    const char *trace = getenv("DKR_TRACE_DRAWS");
    uint64_t trace_frame = trace ? strtoull(trace, NULL, 10) : 0;
    if (g_trace_draws) hos_log("draw: end of traced frame\n");
    g_trace_draws = trace_frame && frame + 1 == trace_frame;
    if (g_trace_draws) hos_log("draw: tracing frame %llu\n", (unsigned long long)trace_frame);
    if (dkr_perf_on) dkr_perf.present += dkr_perf_now() - t0;
    ReleaseSRWLockExclusive(&lock);
    double before_fps = dkr_perf_last_fps;
    dkr_perf_frame();
    if (dkr_perf_on && dkr_perf_last_fps != before_fps) { /* once per second: the frame rate in the window title */
        char title[96];
        snprintf(title, sizeof title, "DKRecomp - %.0f fps", dkr_perf_last_fps);
        dkr_gfx_set_title(s->gfx, title);
    }
}
void dkr_nvn_replay_queue_destroy(void *state) { free(state); }
void dkr_nvn_replay_cleanup(void) {
    while (images) { Image *next = images->next; free(images); images = next; }
    memset(image_buckets, 0, sizeof image_buckets);
    while (stagings) { Staging *next = stagings->next; free(stagings); stagings = next; }
    while (feedback_buffers) { FeedbackBuffer *next = feedback_buffers->next; free(feedback_buffers); feedback_buffers = next; }
    while (samplers) { Sampler *next = samplers->next; free(samplers); samplers = next; }
    for (unsigned i = 0; i < 512; i++) {
        while (stage_info_buckets[i]) { StageInfo *next = stage_info_buckets[i]->next; free(stage_info_buckets[i]); stage_info_buckets[i] = next; }
    }
    while (stage_entries) {
        StageEntry *next = stage_entries->next;
        dkr_shader_cache_free(&stage_entries->cache);
        free(stage_entries); stage_entries = next;
    }
    memset(stage_buckets, 0, sizeof stage_buckets);
    next_stage_id = 0;
    while (programs) { Program *next = programs->next; free(programs); programs = next; }
    memset(program_buckets, 0, sizeof program_buckets);
    nvn_shader_client_shutdown();
}
