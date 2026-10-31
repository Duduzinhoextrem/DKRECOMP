/* Host NVN boundary, called by generated function overrides. No guest object layout is assumed here. */
#ifndef DKR_NVN_H
#define DKR_NVN_H

#include "dkr/rt.h"

/* An original NVN driver routine in the recompiled sdk that only works on its arguments and constant data
 * (dkrecomp.nvnmap --purity). gen.py --gfx writes the list of these to nvn_originals.c. */
typedef struct DkrNvnOriginal {
    const char *name;
    uint64_t address;
} DkrNvnOriginal;

/* Run once before starting guest threads. Procs in `originals` whose category nvn.c accepts (builders, state
 * objects, handle encoders, device mode accessors, debug labels) resolve to the original routine; the rest resolve
 * to host implementations or stop by name. `originals` may be NULL. */
void dkr_nvn_register(const DkrNvnOriginal *originals, size_t count);
void dkr_nvn_graphics_init(Cpu *cpu);
void dkr_nvn_graphics_finalize(Cpu *cpu);
void dkr_nvn_devtools_init(Cpu *cpu);
void dkr_nvn_devtools_finalize(Cpu *cpu);
void dkr_nvn_bootstrap(Cpu *cpu);
uint64_t dkr_nvn_proc_address(const char *name);

#endif
