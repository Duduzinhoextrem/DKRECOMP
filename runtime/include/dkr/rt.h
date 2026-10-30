/* DKRecomp runtime: the parts the host program (not recompiled code) uses. */
#ifndef DKR_RT_H
#define DKR_RT_H

#include "dkr/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One lifted guest function. Generated code defines dkr_fn_table, sorted by `addr`. */
typedef struct DkrFnEntry {
    uint64_t addr;
    GuestFn fn;
} DkrFnEntry;

extern const DkrFnEntry dkr_fn_table[];
extern const size_t dkr_fn_count;

GuestFn rt_lookup(uint64_t addr);

/* Hooks the host may install. Without a hook, SVC/BRK/unsupported instructions print and abort. */
typedef void (*DkrSvcHook)(Cpu *c, uint32_t imm, uint64_t pc);
extern DkrSvcHook dkr_svc_hook;

/* Map `size` bytes of guest memory at guest address == host address `addr`. Returns 0 on success. */
int rt_map_fixed(uint64_t addr, uint64_t size);
void rt_unmap(uint64_t addr);
unsigned long rt_last_os_error(void);

#ifdef __cplusplus
}
#endif
#endif
