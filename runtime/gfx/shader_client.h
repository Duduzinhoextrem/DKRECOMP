#ifndef DKR_SHADER_CLIENT_H
#define DKR_SHADER_CLIENT_H
#include "nvn_shaders.h"
#include "shader_cache.h"

typedef struct NvnShaderInputs {
    uint32_t texture_mask, texture_targets[32], texture_formats[32];
    uint32_t attributes[16], outputs[8]; /* NVNformat, 0 for absent */
    uint32_t primitive;
} NvnShaderInputs;

void nvn_shader_variant_key(Cpu *c, const NvnShader *shader, const NvnShaderInputs *inputs, char key[66]);
void nvn_shader_translate(Cpu *c, const NvnShader *shader, const NvnShaderInputs *inputs, DkrShaderCache *cache);
void nvn_shader_client_shutdown(void);
#endif
