/* Horizon (Switch OS) layer for the first boot attempts: process address space, main thread, the rtld callbacks
 * of nn::init::Start and an SVC handler that logs every call.
 *
 * Boot, as rtld does it on the console (read from the game's own rtld, see docs/PROGRESS.md):
 *   nn::init::Start(main_thread_handle, address_after_rtld, cb_flag, cb_module_inits)
 * cb_flag stores 1 in an rtld variable; cb_module_inits runs every module's DT_INIT in load order. Start then
 * initialises the SDK and calls nnMain. rtld is not loaded: tools/dkrecomp/link.py did its linking at build time.
 *
 * SVCs are implemented only as far as a single-threaded boot needs them. Anything else stops the run with a
 * report, so each attempt shows exactly where the next piece of work is. Register conventions follow switchbrew
 * (SVC page): outputs come back in w0 (result) and x1.. as the libnx wrappers expect. */
#include "dkr/rt.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern const DkrModule dkr_modules[];
extern const size_t dkr_module_count;
extern const uint64_t dkr_sym_init_start, dkr_sym_nn_main, dkr_main_thread_stack_size;

/* ---- address space (39-bit; regions chosen for DKRecomp, reported to the guest through GetInfo) ------------ */
#define ASLR_BASE 0x0008000000ull
#define ASLR_SIZE 0x7FF8000000ull
#define HEAP_BASE 0x1000000000ull
#define HEAP_SIZE 0x0180000000ull
#define ALIAS_BASE 0x2000000000ull
#define ALIAS_SIZE 0x1000000000ull
#define STACK_BASE 0x3000000000ull
#define STACK_SIZE 0x0080000000ull
#define TLR_BASE 0x3800000000ull
/* Memory available to an application on a 4 GiB console (switchbrew, "Memory layout"): an approximation. */
#define TOTAL_MEMORY 0xCD500000ull
#define MAIN_THREAD_HANDLE 0xDE00u
#define CUR_PROCESS 0xFFFF8001u
#define CUR_THREAD 0xFFFF8000u

/* Result codes (module 1 = kernel; value = module | description << 9). */
#define R_OK 0
#define R_TIMED_OUT 0xEA01
#define R_INVALID_HANDLE 0xE401
#define R_INVALID_ENUM 0xF001

/* MemoryState values of firmware 4.x (switchbrew "SVC#MemoryState"), lower byte only. */
enum { MS_FREE = 0, MS_CODE = 3, MS_CODEDATA = 4, MS_NORMAL = 5, MS_STACK = 11, MS_THREADLOCAL = 12 };

typedef struct Region {
    uint64_t base, size;
    uint32_t state, perm;
} Region;
static Region g_regions[256];
static int g_nregions;
static uint64_t g_heap_size;

static void region_add(uint64_t base, uint64_t size, uint32_t state, uint32_t perm) {
    int i = g_nregions++;
    while (i > 0 && g_regions[i - 1].base > base) { g_regions[i] = g_regions[i - 1]; i--; }
    g_regions[i] = (Region){base, size, state, perm};
}

/* ---- logging and stopping --------------------------------------------------------------------------------- */
static uint64_t g_svc_count;
static FILE *g_log;

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

static void stop(Cpu *c, const char *why) {
    char a[96];
    logf_("\nboot: STOP after %llu SVCs: %s\n", (unsigned long long)g_svc_count, why);
    logf_("boot: lr=%s sp=%#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx\n", rt_describe(c->x[30], a, sizeof a),
          (unsigned long long)c->sp, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
          (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
    if (g_log) fclose(g_log);
    exit(3);
}

/* ---- SVCs ------------------------------------------------------------------------------------------------- */
static const char *svc_name(uint32_t n) {
    static const char *names[0x80] = {
        [0x01] = "SetHeapSize", [0x02] = "SetMemoryPermission", [0x03] = "SetMemoryAttribute",
        [0x04] = "MapMemory", [0x05] = "UnmapMemory", [0x06] = "QueryMemory", [0x07] = "ExitProcess",
        [0x08] = "CreateThread", [0x09] = "StartThread", [0x0A] = "ExitThread", [0x0B] = "SleepThread",
        [0x0C] = "GetThreadPriority", [0x0D] = "SetThreadPriority", [0x0E] = "GetThreadCoreMask",
        [0x0F] = "SetThreadCoreMask", [0x10] = "GetCurrentProcessorNumber", [0x11] = "SignalEvent",
        [0x12] = "ClearEvent", [0x13] = "MapSharedMemory", [0x14] = "UnmapSharedMemory",
        [0x15] = "CreateTransferMemory", [0x16] = "CloseHandle", [0x17] = "ResetSignal",
        [0x18] = "WaitSynchronization", [0x19] = "CancelSynchronization", [0x1A] = "ArbitrateLock",
        [0x1B] = "ArbitrateUnlock", [0x1C] = "WaitProcessWideKeyAtomic", [0x1D] = "SignalProcessWideKey",
        [0x1E] = "GetSystemTick", [0x1F] = "ConnectToNamedPort", [0x21] = "SendSyncRequest",
        [0x22] = "SendSyncRequestWithUserBuffer", [0x24] = "GetProcessId", [0x25] = "GetThreadId",
        [0x26] = "Break", [0x27] = "OutputDebugString", [0x28] = "ReturnFromException", [0x29] = "GetInfo",
        [0x2C] = "MapPhysicalMemory", [0x2D] = "UnmapPhysicalMemory", [0x32] = "SetThreadActivity",
        [0x33] = "GetThreadContext3", [0x5F] = "FlushProcessDataCache"};
    return n < 0x80 && names[n] ? names[n] : "?";
}

static uint64_t get_info(uint32_t id0, uint32_t handle, uint64_t id1, int *ok) {
    *ok = 1;
    switch (id0) {
    case 0: return 0xF;                       /* core mask: cores 0-3 */
    case 1: return 0xFFFFFFFFFFFFFFFFull;     /* priority mask */
    case 2: return ALIAS_BASE;
    case 3: return ALIAS_SIZE;
    case 4: return HEAP_BASE;
    case 5: return HEAP_SIZE;
    case 6: return TOTAL_MEMORY;
    case 7: return g_heap_size + 0x08000000;  /* used memory: heap + an estimate for code and stacks */
    case 8: return 0;                          /* debugger attached */
    case 11: return 0x0123456789ABCDEFull ^ (id1 * 0x9E3779B97F4A7C15ull); /* random entropy (fixed for reproducibility) */
    case 12: return ASLR_BASE;
    case 13: return ASLR_SIZE;
    case 14: return STACK_BASE;
    case 15: return STACK_SIZE;
    case 16: return 0;                         /* system resource size total */
    case 17: return 0;
    case 18: return 0x0100C1F0051B6000ull;     /* program id (main.npdm ACI0) */
    case 20: return 0;                         /* user exception context address */
    case 21: return TOTAL_MEMORY;
    case 22: return g_heap_size + 0x08000000;
    case 23: return 1;                         /* is application */
    default: (void)handle; *ok = 0; return 0;
    }
}

static void svc_handler(Cpu *c, uint32_t n, uint64_t pc) {
    char where[96];
    g_svc_count++;
    rt_describe(pc, where, sizeof where);
    logf_("svc %4llu  %-26s 0x%02x at %-18s x0=%#llx x1=%#llx x2=%#llx x3=%#llx\n", (unsigned long long)g_svc_count,
          svc_name(n), n, where, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
          (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
    switch (n) {
    case 0x01: { /* SetHeapSize(size x1) -> x1 address */
        uint64_t size = c->x[1];
        if (size > HEAP_SIZE || (size & 0x1FFFFF)) stop(c, "SetHeapSize: size beyond the heap region or unaligned");
        if (size > g_heap_size && rt_map_fixed(HEAP_BASE + g_heap_size, size - g_heap_size))
            stop(c, "SetHeapSize: host mapping failed");
        if (size > g_heap_size) region_add(HEAP_BASE + g_heap_size, size - g_heap_size, MS_NORMAL, 3);
        else if (size < g_heap_size) stop(c, "SetHeapSize: shrinking the heap is not implemented");
        g_heap_size = size;
        c->x[0] = R_OK;
        c->x[1] = HEAP_BASE;
        return;
    }
    case 0x02: case 0x03: c->x[0] = R_OK; return; /* memory permission / attribute: accepted, not enforced */
    case 0x06: { /* QueryMemory(MemoryInfo* x0, -, addr x2) -> w1 page info */
        uint64_t addr = c->x[2], lo = 0, hi = 1ull << 39;
        uint32_t state = MS_FREE, perm = 0;
        for (int i = 0; i < g_nregions; i++) {
            const Region *r = &g_regions[i];
            if (addr >= r->base && addr < r->base + r->size) { lo = r->base; hi = r->base + r->size; state = r->state; perm = r->perm; break; }
            if (r->base + r->size <= addr && r->base + r->size > lo) lo = r->base + r->size;
            if (r->base > addr && r->base < hi) hi = r->base;
        }
        uint8_t *mi = (uint8_t *)(uintptr_t)c->x[0];
        memset(mi, 0, 0x28);
        memcpy(mi + 0x00, &lo, 8);
        uint64_t size = hi - lo;
        memcpy(mi + 0x08, &size, 8);
        memcpy(mi + 0x10, &state, 4);
        memcpy(mi + 0x18, &perm, 4);
        c->x[0] = R_OK;
        c->x[1] = 0;
        return;
    }
    case 0x0B: /* SleepThread(ns x0) */
#ifdef _WIN32
        Sleep((DWORD)(c->x[0] / 1000000));
#endif
        c->x[0] = R_OK;
        return;
    case 0x0C: c->x[0] = R_OK; c->x[1] = 44; return;          /* GetThreadPriority: main.npdm priority */
    case 0x0D: c->x[0] = R_OK; return;                        /* SetThreadPriority */
    case 0x0E: c->x[0] = R_OK; c->x[1] = 0; c->x[2] = 0xF; return; /* GetThreadCoreMask */
    case 0x0F: c->x[0] = R_OK; return;                        /* SetThreadCoreMask */
    case 0x10: c->x[0] = 0; return;                           /* GetCurrentProcessorNumber: core 0 */
    case 0x11: case 0x12: case 0x16: case 0x17: case 0x19: c->x[0] = R_OK; return;
    case 0x1A: c->x[0] = R_OK; return; /* ArbitrateLock: one thread, so the owner released the lock already */
    case 0x1B: /* ArbitrateUnlock(mutex x0): no waiters, so the mutex word becomes 0 */
        st32(c->x[0], 0);
        c->x[0] = R_OK;
        return;
    case 0x1D: c->x[0] = R_OK; return; /* SignalProcessWideKey: nobody waits */
    case 0x1E: c->x[0] = rt_cntvct(); return;
    case 0x1F: { /* ConnectToNamedPort(-, name x1) -> w1 handle */
        char name[16] = {0};
        memcpy(name, (const void *)(uintptr_t)c->x[1], 12);
        logf_("     port \"%s\"\n", name);
        c->x[0] = R_OK;
        c->x[1] = 0xD000;
        return;
    }
    case 0x21: { /* SendSyncRequest(handle w0): the IPC message is in the thread local region */
        const uint32_t *msg = (const uint32_t *)(uintptr_t)c->tpidrro;
        logf_("     IPC to handle %#x: %08x %08x %08x %08x %08x %08x %08x %08x\n", (unsigned)c->x[0], msg[0], msg[1],
              msg[2], msg[3], msg[4], msg[5], msg[6], msg[7]);
        stop(c, "IPC (SendSyncRequest) is not implemented yet");
    }
    case 0x24: c->x[0] = R_OK; c->x[1] = 0x51; return; /* GetProcessId */
    case 0x25: c->x[0] = R_OK; c->x[1] = 1; return;    /* GetThreadId */
    case 0x26: stop(c, "the guest called Break");
    case 0x27: { /* OutputDebugString(ptr x0, len x1) */
        uint64_t len = c->x[1] > 4096 ? 4096 : c->x[1];
        logf_("     debug: %.*s\n", (int)len, (const char *)(uintptr_t)c->x[0]);
        c->x[0] = R_OK;
        return;
    }
    case 0x29: { /* GetInfo(-, id0 w1, handle w2, id1 x3) -> x1 */
        int ok;
        uint64_t v = get_info((uint32_t)c->x[1], (uint32_t)c->x[2], c->x[3], &ok);
        if (!ok) {
            char why[96];
            snprintf(why, sizeof why, "GetInfo id %llu is not implemented", (unsigned long long)(uint32_t)c->x[1]);
            stop(c, why);
        }
        logf_("     GetInfo(%u, %llu) = %#llx\n", (unsigned)c->x[1], (unsigned long long)c->x[3], (unsigned long long)v);
        c->x[0] = R_OK;
        c->x[1] = v;
        return;
    }
    case 0x5F: c->x[0] = R_OK; return; /* FlushProcessDataCache */
    default: {
        char why[96];
        snprintf(why, sizeof why, "SVC 0x%02x %s is not implemented yet", n, svc_name(n));
        stop(c, why);
    }
    }
}

/* ---- rtld callbacks and boot -------------------------------------------------------------------------------- */
static void cb_flag(Cpu *c) {
    (void)c;
    logf_("boot: rtld callback 1 (sets rtld's ready flag)\n");
}

static void cb_module_inits(Cpu *c) {
    Cpu saved = *c; /* DT_INIT follows the AAPCS64; the callback returns to guest code with its state intact */
    for (size_t i = 0; i < dkr_module_count; i++) {
        const DkrModule *m = &dkr_modules[i];
        if (!m->init) continue;
        char a[96];
        logf_("boot: DT_INIT of %s at %s\n", m->name, rt_describe(m->init, a, sizeof a));
        rt_call(c, m->init);
        logf_("boot: DT_INIT of %s returned\n", m->name);
    }
    for (int r = 19; r <= 30; r++) c->x[r] = saved.x[r];
    c->sp = saved.sp;
}

static void cb_returned(Cpu *c) { stop(c, "guest code returned to the host return address"); }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    g_log = fopen("dkctf_boot.log", "w");
    if (rt_load_modules(dkr_modules, dkr_module_count, dir)) return 2;
    for (size_t i = 0; i < dkr_module_count; i++) {
        const DkrModule *m = &dkr_modules[i];
        region_add(m->base, m->text_hi - m->base, MS_CODE, 5);
        region_add(m->text_hi, m->base + m->size - m->text_hi, MS_CODEDATA, 3);
    }
    /* main thread: stack at the bottom of the stack region, thread local region (0x200 bytes) in its own page */
    uint64_t stack = dkr_main_thread_stack_size;
    if (rt_map_fixed(STACK_BASE, stack) || rt_map_fixed(TLR_BASE, 0x1000)) {
        fprintf(stderr, "boot: cannot map the main thread's stack or TLS\n");
        return 2;
    }
    region_add(STACK_BASE, stack, MS_STACK, 3);
    region_add(TLR_BASE, 0x1000, MS_THREADLOCAL, 3);
    dkr_svc_hook = svc_handler;

    static Cpu cpu;
    Cpu *c = &cpu;
    c->sp = STACK_BASE + stack;
    c->tpidrro = TLR_BASE;
    c->x[0] = MAIN_THREAD_HANDLE;
    c->x[1] = dkr_modules[0].base; /* rtld passes the address right after itself: the first module's base */
    c->x[2] = rt_hostfn_register("rtld flag callback", cb_flag);
    c->x[3] = rt_hostfn_register("rtld module initializers", cb_module_inits);
    c->x[30] = rt_hostfn_register("return to host", cb_returned);
    char a[96];
    logf_("boot: calling nn::init::Start at %s (nnMain at %s)\n", rt_describe(dkr_sym_init_start, a, sizeof a),
          rt_describe(dkr_sym_nn_main, a + 48, 48));
    rt_call(c, dkr_sym_init_start);
    stop(c, "nn::init::Start returned");
    return 0;
}
