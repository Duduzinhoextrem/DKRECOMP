/* Versioned bridge emitted by our managed translator. This is resource metadata, not a SPIR-V validator. */
#ifndef DKR_SHADER_CACHE_H
#define DKR_SHADER_CACHE_H
#include <stddef.h>
#include <stdint.h>

enum { DKR_SHADER_SUPPORT, DKR_SHADER_CBUF, DKR_SHADER_SBUF, DKR_SHADER_TEXTURE,
       DKR_SHADER_IMAGE, DKR_SHADER_SAMPLER, DKR_SHADER_STORAGE_IMAGE, DKR_SHADER_TEXEL, DKR_SHADER_STORAGE_TEXEL };
typedef struct DkrShaderBinding {
    uint32_t binding, kind;
    int32_t slot, cb_slot, handle;
    uint32_t texture_type, count, flags;
} DkrShaderBinding;
typedef struct DkrShaderCache {
    uint32_t stage, support_size, binding_count, spv_size, notes;
    uint8_t compiler[32];
    DkrShaderBinding bindings[128];
    uint32_t *spv;
} DkrShaderCache;

int dkr_shader_cache_parse(const void *data, size_t size, DkrShaderCache *out, char error[256]);
void dkr_shader_cache_free(DkrShaderCache *cache);
#endif
