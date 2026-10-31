/* DKRecomp runtime: the parts the host program (not recompiled code) uses. */
#ifndef DKR_RT_H
#define DKR_RT_H

#include "dkr/cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One lifted guest function. Each generated module provides a table sorted by `addr`. */
typedef struct DkrFnEntry {
    uint64_t addr;
    GuestFn fn;
} DkrFnEntry;

/* A host override can explicitly delegate to the preserved guest implementation. */
typedef struct DkrOriginal {
    const char *host;
    uint64_t addr;
    GuestFn fn;
} DkrOriginal;

/* Indirect branches look addresses up in the registered tables, then in the host functions. */
void rt_register_table(const DkrFnEntry *table, size_t count, uint64_t lo, uint64_t hi);
GuestFn rt_lookup(uint64_t addr);

/* Host functions callable from guest code: rt_hostfn_register returns a guest address (outside every module)
 * whose indirect call runs `fn`. */
#define DKR_HOSTFN_BASE 0x7F00000000ull
uint64_t rt_hostfn_register(const char *name, GuestFn fn);
const char *rt_hostfn_name(uint64_t addr);

/* Call guest code from the host: x0..x7 = args, then returns when the guest function returns. */
void rt_call(Cpu *c, uint64_t addr);
const DkrOriginal *rt_original(const char *host);
void rt_call_original(Cpu *c, const char *host);

/* A generated, statically linked module (tools/dkrecomp/gen.py writes the table of these in layout.c). */
typedef struct DkrModule {
    const char *name;
    uint64_t base, size;       /* mapped range: image + bss */
    uint64_t image_size;       /* bytes stored in <name>.img */
    uint64_t text_lo, text_hi;
    uint64_t init;             /* DT_INIT, 0 if none */
    const DkrFnEntry *table;
    const size_t *count;
    const DkrOriginal *originals;
    const size_t *original_count;
} DkrModule;

/* SVC dispatch: the Horizon layer installs its handler here. */
typedef void (*DkrSvcHook)(Cpu *c, uint32_t imm, uint64_t pc);
extern DkrSvcHook dkr_svc_hook;

/* Map `size` bytes of guest memory at guest address == host address `addr`. Returns 0 on success. */
int rt_map_fixed(uint64_t addr, uint64_t size);
void rt_unmap(uint64_t addr);
void rt_decommit(uint64_t addr, uint64_t size); /* give back the host memory of a mapped range */
unsigned long rt_last_os_error(void);

/* Load every module's image at its base and register its dispatch table. Returns 0 on success. */
int rt_load_modules(const DkrModule *mods, size_t count, const char *dir);

/* Describe a guest address ("main+0x1234") for logs. */
const char *rt_describe(uint64_t addr, char *buf, size_t n);
void rt_set_modules(const DkrModule *mods, size_t count);

#ifdef __cplusplus
}
#endif
#endif
