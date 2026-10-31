/* Host NVN boundary, called by generated function overrides. No guest object layout is assumed here. */
#ifndef DKR_NVN_H
#define DKR_NVN_H

#include "dkr/rt.h"

void dkr_nvn_register(void); /* run once before starting guest threads */
void dkr_nvn_graphics_init(Cpu *cpu);
void dkr_nvn_graphics_finalize(Cpu *cpu);
void dkr_nvn_devtools_init(Cpu *cpu);
void dkr_nvn_devtools_finalize(Cpu *cpu);
void dkr_nvn_bootstrap(Cpu *cpu);
uint64_t dkr_nvn_proc_address(const char *name);

#endif
