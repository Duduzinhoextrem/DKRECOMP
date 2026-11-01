/* Shared by the NVN host files (nvn.c: device and proc registry; nvn_obj.c: objects). */
#ifndef DKR_NVN_INTERNAL_H
#define DKR_NVN_INTERNAL_H

#include "dkr/rt.h"
#include <stddef.h>
#include "nvn_tex.h"
#include "dkr/gfx.h"

typedef struct NvnImpl {
    const char *name;
    GuestFn fn;
} NvnImpl;

extern const NvnImpl dkr_nvn_obj_impls[];
extern const size_t dkr_nvn_obj_impl_count;
GuestFn dkr_nvn_obj_fallback(const char *name); /* generic handler for a family (command recording), or NULL */

/* Guest address of the original pure driver routine for `name` (any category), 0 if the analysis did not list it. */
uint64_t dkr_nvn_original(const char *name);

typedef struct NvnTextureInfo {
    NvnTexDesc desc;
    NvnTexLayout layout;
    uint64_t address;
    uint32_t serial, format, target, swizzle[4], ds_mode;
    uint32_t base_level, levels, base_layer, layers;
} NvnTextureInfo;
void dkr_nvn_texture_info(Cpu *cpu, uint64_t texture, const void *view, NvnTextureInfo *out);
void dkr_nvn_texture_binding(Cpu *cpu, uint64_t pool, unsigned id, NvnTextureInfo *out);
void dkr_nvn_sampler_binding(Cpu *cpu, uint64_t pool, unsigned id, uint8_t builder[0x48]);
DkrGfx *dkr_nvn_graphics(void);

#endif
