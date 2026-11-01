/* Owned shader programs at the NVN/Vulkan boundary. No pointers into transient compiler output are retained. */
#ifndef DKR_NVN_SHADERS_H
#define DKR_NVN_SHADERS_H
#include "dkr/rt.h"

typedef struct NvnShader {
    uint32_t stage, control_size, code_size, c1_offset;
    uint8_t *control, *code;
    char key[33]; /* first 128 bits of SHA-256(control || code), matching shaderjobs */
} NvnShader;

typedef struct NvnProgramSnapshot {
    unsigned count;
    NvnShader shaders[6];
} NvnProgramSnapshot;

void nvn_shader_snapshot(Cpu *c, NvnShader *out, uint64_t control, uint64_t code);
void nvn_shader_release(NvnShader *shader);
void nvn_program_capture(Cpu *c, const NvnProgramSnapshot *program);
const NvnProgramSnapshot *dkr_nvn_program_snapshot(Cpu *c, uint64_t program);
#endif
