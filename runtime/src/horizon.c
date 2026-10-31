/* Horizon (Switch OS) layer for the first boot attempts: process address space, main thread, the rtld callbacks
 * of nn::init::Start and an SVC handler that logs every call.
 *
 * Boot, as rtld does it on the console (read from the game's own rtld, see docs/PROGRESS.md):
 *   nn::init::Start(main_thread_handle, address_after_rtld, cb_flag, cb_module_inits)
 * cb_flag stores 1 in an rtld variable; cb_module_inits runs every module's DT_INIT in load order. Start then
 * initialises the SDK and calls nnMain. rtld is not loaded: tools/dkrecomp/link.py did its linking at build time.
 *
 * SVCs are implemented as the boot needs them. Anything else stops the run with a
 * report, so each attempt shows exactly where the next piece of work is. Register conventions follow switchbrew
 * (SVC page): outputs come back in w0 (result) and x1.. as the libnx wrappers expect. */
#include "hos.h"
#ifdef DKR_ENABLE_GFX
#include "dkr/nvn.h"
#endif

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
#define PROGRAM_ID 0x0100C1F0051B6000ull

/* Result codes (module 1 = kernel; value = module | description << 9). */
#define R_OK 0
#define R_TIMED_OUT 0xEA01
#define R_INVALID_HANDLE 0xE401
#define R_INVALID_ENUM 0xF001

/* MemoryState values of firmware 4.x (switchbrew "SVC#MemoryState"), lower byte only. */
enum { MS_FREE = 0, MS_CODE = 3, MS_CODEDATA = 4, MS_NORMAL = 5, MS_SHARED = 6, MS_STACK = 11, MS_THREADLOCAL = 12 };

typedef struct Region {
    uint64_t base, size;
    uint32_t state, perm;
} Region;
static Region g_regions[256];
static int g_nregions;
static uint64_t g_heap_size;

static uint64_t g_fixed_used; /* modules, stacks and thread local regions: memory the heap cannot use */

static void region_add(uint64_t base, uint64_t size, uint32_t state, uint32_t perm) {
    int i = g_nregions++;
    while (i > 0 && g_regions[i - 1].base > base) { g_regions[i] = g_regions[i - 1]; i--; }
    g_regions[i] = (Region){base, size, state, perm};
}

/* Replace whatever is mapped in [base, base + span) by one region of `size` bytes (0 removes it). */
static void region_set(uint64_t base, uint64_t span, uint64_t size, uint32_t state, uint32_t perm) {
    int j = 0;
    for (int i = 0; i < g_nregions; i++)
        if (g_regions[i].base < base || g_regions[i].base >= base + span) g_regions[j++] = g_regions[i];
    g_nregions = j;
    if (size) region_add(base, size, state, perm);
}

/* ---- logging and stopping --------------------------------------------------------------------------------- */
static uint64_t g_svc_count;
static FILE *g_log;

void hos_log(const char *fmt, ...) {
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

void hos_stop(Cpu *c, const char *fmt, ...) {
    char a[96], why[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, sizeof why, fmt, ap);
    va_end(ap);
    hos_log("\nboot: STOP after %llu SVCs: %s\n", (unsigned long long)g_svc_count, why);
    hos_log("boot: lr=%s sp=%#llx x0=%#llx x1=%#llx x2=%#llx x3=%#llx\n", rt_describe(c->x[30], a, sizeof a),
          (unsigned long long)c->sp, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
          (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
#ifdef _WIN32
    uint64_t fp = c->x[29];
    for (int depth = 0; depth < 20 && fp && !(fp & 7); depth++) {
        uint64_t frame[2];
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)fp, frame, sizeof frame, &read) ||
            read != sizeof frame) break;
        hos_log("boot: frame %d fp=%#llx lr=%s\n", depth, (unsigned long long)fp,
                rt_describe(frame[1], a, sizeof a));
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
#endif
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
    case 7: return g_fixed_used + g_heap_size; /* used memory: modules, stacks, heap */
    case 8: return 0;                          /* debugger attached */
    case 11: return 0x0123456789ABCDEFull ^ (id1 * 0x9E3779B97F4A7C15ull); /* random entropy (fixed for reproducibility) */
    case 12: return ASLR_BASE;
    case 13: return ASLR_SIZE;
    case 14: return STACK_BASE;
    case 15: return STACK_SIZE;
    case 16: return 0;                         /* system resource size total */
    case 17: return 0;
    case 18: return PROGRAM_ID;             /* program id (main.npdm ACI0) */
    case 20: return 0;                         /* user exception context address */
    case 21: return TOTAL_MEMORY;
    case 22: return g_fixed_used + g_heap_size;
    case 23: return 1;                         /* is application */
    default: (void)handle; *ok = 0; return 0;
    }
}

/* Synchronisation SVCs are frequent and not logged unless DKR_LOG_SYNC is set; everything else is. */
static int quiet_svc(uint32_t n) {
    static int verbose = -1;
    if (verbose < 0) verbose = getenv("DKR_LOG_SYNC") != NULL;
    if (verbose) return 0;
    return n == 0x0B || n == 0x10 || n == 0x18 || n == 0x1A || n == 0x1B || n == 0x1C || n == 0x1D || n == 0x1E;
}

static void svc_dispatch(Cpu *c, uint32_t n, uint64_t pc) {
    char where[96];
    g_svc_count++;
    kern_note_svc(svc_name(n), pc);
    if (!quiet_svc(n)) {
        rt_describe(pc, where, sizeof where);
        hos_log("svc %5llu t%x %-26s 0x%02x at %-18s x0=%#llx x1=%#llx x2=%#llx x3=%#llx\n",
                (unsigned long long)g_svc_count, kern_current_handle(), svc_name(n), n, where,
                (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                (unsigned long long)c->x[3]);
    }
    switch (n) {
    case 0x01: { /* SetHeapSize(size x1) -> x1 address */
        uint64_t size = c->x[1];
        if (size & 0x1FFFFF) { c->x[0] = HOS_KERNEL(102); return; } /* InvalidSize: must be 2 MiB aligned */
        /* The process may use at most TOTAL_MEMORY for modules, stacks and heap together; the SDK probes the
         * largest heap it can get, so a request above that must fail like the kernel's (OutOfMemory). */
        if (size > HEAP_SIZE || g_fixed_used + size > TOTAL_MEMORY) {
            hos_log("     heap of %#llx refused: out of memory\n", (unsigned long long)size);
            c->x[0] = HOS_KERNEL(104);
            return;
        }
        if (size > g_heap_size && rt_map_fixed(HEAP_BASE + g_heap_size, size - g_heap_size))
            hos_stop(c, "SetHeapSize: host mapping failed");
        if (size < g_heap_size) rt_decommit(HEAP_BASE + size, g_heap_size - size);
        g_heap_size = size;
        region_set(HEAP_BASE, HEAP_SIZE, size, MS_NORMAL, 3);
        c->x[0] = R_OK;
        c->x[1] = HEAP_BASE;
        return;
    }
    case 0x02: case 0x03: c->x[0] = R_OK; return; /* memory permission / attribute: accepted, not enforced */
    case 0x04: { /* MapMemory(dst x0, src x1, size x2): nn::os uses it to place thread stacks in the stack region.
                  * The kernel aliases the pages and makes src inaccessible until UnmapMemory, so copying src to
                  * dst here and back on unmap is equivalent: nothing can use src in between. */
        uint64_t dst = c->x[0], src = c->x[1], size = c->x[2];
        if ((dst | src | size) & 0xFFF || dst < STACK_BASE || dst + size > STACK_BASE + STACK_SIZE)
            hos_stop(c, "MapMemory: unaligned or outside the stack region");
        if (rt_map_fixed(dst, size)) hos_stop(c, "MapMemory: host mapping failed");
        memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)size);
        region_add(dst, size, MS_STACK, 3);
        c->x[0] = R_OK;
        return;
    }
    case 0x05: { /* UnmapMemory(dst x0, src x1, size x2): copy back, release dst */
        uint64_t dst = c->x[0], src = c->x[1], size = c->x[2];
        memcpy((void *)(uintptr_t)src, (const void *)(uintptr_t)dst, (size_t)size);
        rt_decommit(dst, size);
        region_set(dst, size, 0, 0, 0);
        c->x[0] = R_OK;
        return;
    }
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
    case 0x08: { /* CreateThread(-, entry x1, arg x2, stack top x3, priority w4, core w5) -> w1 handle */
        uint32_t h = 0;
        uint64_t entry = c->x[1];
        char e[96];
        c->x[0] = kern_create_thread(entry, c->x[2], c->x[3], (int)(int32_t)c->x[4], (int)(int32_t)c->x[5], &h);
        c->x[1] = h;
        hos_log("     thread %#x: entry %s, priority %d, core %d\n", h, rt_describe(entry, e, sizeof e),
                (int)(int32_t)c->x[4], (int)(int32_t)c->x[5]);
        return;
    }
    case 0x09: c->x[0] = kern_start_thread((uint32_t)c->x[0]); return; /* StartThread(handle) */
    case 0x0A: /* ExitThread */
        kern_exit_current();
        kern_unlock();
        ExitThread(0);
    case 0x0B: kern_sleep((int64_t)c->x[0]); c->x[0] = R_OK; return; /* SleepThread(ns) */
    case 0x0C: { int p = 0; c->x[0] = kern_thread_prio((uint32_t)c->x[1], 0, 0, &p); c->x[1] = (uint32_t)p; return; }
    case 0x0D: c->x[0] = kern_thread_prio((uint32_t)c->x[0], 1, (int)(int32_t)c->x[1], NULL); return;
    case 0x0E: { /* GetThreadCoreMask(-, -, handle w2) -> w1 core, x2 mask */
        int core = 0;
        c->x[0] = kern_thread_core((uint32_t)c->x[2], 0, 0, &core);
        c->x[1] = (uint32_t)core;
        c->x[2] = 0xF;
        return;
    }
    case 0x0F: c->x[0] = kern_thread_core((uint32_t)c->x[0], 1, (int)(int32_t)c->x[1], NULL); return;
    case 0x10: c->x[0] = (uint64_t)kern_current_core(); return; /* GetCurrentProcessorNumber */
    case 0x11: hos_event_set((uint32_t)c->x[0], 1); c->x[0] = R_OK; return;          /* SignalEvent */
    case 0x12: case 0x17: hos_event_set((uint32_t)c->x[0], 0); c->x[0] = R_OK; return; /* ClearEvent, ResetSignal */
    case 0x13: { /* MapSharedMemory(handle w0, addr x1, size x2, permission w3) */
        uint64_t addr = c->x[1], size = c->x[2];
        for (int i = 0; i < g_nregions; i++) {
            Region *r = &g_regions[i];
            if (size && addr < r->base + r->size && r->base < addr + size) {
                c->x[0] = HOS_KERNEL(106);
                return;
            }
        }
        c->x[0] = hos_map_shared_memory((uint32_t)c->x[0], addr, size, (uint32_t)c->x[3]);
        if (c->x[0] == HOS_OK) region_add(addr, size, MS_SHARED, (uint32_t)c->x[3]);
        return;
    }
    case 0x14: { /* UnmapSharedMemory(handle w0, addr x1, size x2) */
        uint64_t addr = c->x[1], size = c->x[2];
        c->x[0] = hos_unmap_shared_memory((uint32_t)c->x[0], addr, size);
        if (c->x[0] == HOS_OK) region_set(addr, size, 0, 0, 0);
        return;
    }
    case 0x15: { /* CreateTransferMemory(-, addr x1, size x2, perm w3) -> w1 handle */
        uint64_t addr = c->x[1], size = c->x[2];
        uint32_t perm = (uint32_t)c->x[3];
        if (addr & 0xfff) { c->x[0] = HOS_KERNEL(102); return; }
        if (!size || (size & 0xfff) || addr + size < addr) { c->x[0] = HOS_KERNEL(101); return; }
        if (perm != 0 && perm != 1 && perm != 3) { c->x[0] = HOS_KERNEL(108); return; }
        uint32_t handle = hos_new_transfer_memory(addr, size, perm);
        c->x[0] = handle ? HOS_OK : HOS_KERNEL(105);
        c->x[1] = handle;
        return;
    }
    case 0x16: c->x[0] = hos_close_handle((uint32_t)c->x[0]); return;                  /* CloseHandle */
    case 0x18: { /* WaitSynchronization(-, handles x1, count w2, timeout x3) -> w1 index */
        int32_t index = -1;
        c->x[0] = kern_wait_sync((const uint32_t *)(uintptr_t)c->x[1], (int)(int32_t)c->x[2], (int64_t)c->x[3], &index);
        c->x[1] = (uint32_t)index;
        return;
    }
    case 0x19: c->x[0] = R_OK; return; /* CancelSynchronization */
    case 0x1A: c->x[0] = kern_arbitrate_lock((uint32_t)c->x[0], c->x[1], (uint32_t)c->x[2]); return;
    case 0x1B: c->x[0] = kern_arbitrate_unlock(c->x[0]); return;
    case 0x1C: c->x[0] = kern_wait_cv(c->x[0], c->x[1], (uint32_t)c->x[2], (int64_t)c->x[3]); return;
    case 0x1D: c->x[0] = kern_signal_cv(c->x[0], (int32_t)c->x[1]); return;
    case 0x1E: c->x[0] = rt_cntvct(); return;
    case 0x1F: { /* ConnectToNamedPort(-, name x1) -> w1 handle */
        char name[16] = {0};
        memcpy(name, (const void *)(uintptr_t)c->x[1], 12);
        hos_log("     port \"%s\"\n", name);
        if (strcmp(name, "sm:")) hos_stop(c, "ConnectToNamedPort(\"%s\"): only sm: is known", name);
        c->x[0] = R_OK;
        c->x[1] = hos_new_session(hos_service_sm());
        return;
    }
    case 0x21: /* SendSyncRequest(handle w0): the message is in the thread local region */
        c->x[0] = hos_send_sync_request(c, (uint32_t)c->x[0], (uint8_t *)(uintptr_t)c->tpidrro, 0x100);
        return;
    case 0x22: /* SendSyncRequestWithUserBuffer(buffer x0, size x1, handle w2) */
        c->x[0] = hos_send_sync_request(c, (uint32_t)c->x[2], (uint8_t *)(uintptr_t)c->x[0], (uint32_t)c->x[1]);
        return;
    case 0x24: c->x[0] = R_OK; c->x[1] = 0x51; return; /* GetProcessId */
    case 0x25: { uint64_t id = 0; c->x[0] = kern_thread_id((uint32_t)c->x[1], &id); c->x[1] = id; return; }
    case 0x26: hos_stop(c, "the guest called Break (reason %#llx)", (unsigned long long)c->x[0]);
    case 0x27: { /* OutputDebugString(ptr x0, len x1) */
        uint64_t len = c->x[1] > 4096 ? 4096 : c->x[1];
        hos_log("     debug: %.*s\n", (int)len, (const char *)(uintptr_t)c->x[0]);
        c->x[0] = R_OK;
        return;
    }
    case 0x29: { /* GetInfo(-, id0 w1, handle w2, id1 x3) -> x1 */
        int ok;
        uint64_t v = get_info((uint32_t)c->x[1], (uint32_t)c->x[2], c->x[3], &ok);
        if (!ok) hos_stop(c, "GetInfo id %llu is not implemented", (unsigned long long)(uint32_t)c->x[1]);
        hos_log("     GetInfo(%u, %llu) = %#llx\n", (unsigned)c->x[1], (unsigned long long)c->x[3], (unsigned long long)v);
        c->x[0] = R_OK;
        c->x[1] = v;
        return;
    }
    case 0x5F: c->x[0] = R_OK; return; /* FlushProcessDataCache */
    default: hos_stop(c, "SVC 0x%02x %s is not implemented yet", n, svc_name(n));
    }
}

static void svc_handler(Cpu *c, uint32_t n, uint64_t pc) {
    kern_lock();
    svc_dispatch(c, n, pc);
    kern_unlock();
}

/* ---- rtld callbacks and boot -------------------------------------------------------------------------------- */
void dkr_boot_entry(Cpu *c, uint64_t entry) {
    (void)c;
    char where[96];
    hos_log("boot: entered nnMain at %s on thread %#x\n", rt_describe(entry, where, sizeof where), kern_current_handle());
}

static void cb_flag(Cpu *c) {
    (void)c;
    hos_log("boot: rtld callback 1 (sets rtld's ready flag)\n");
}

static void cb_module_inits(Cpu *c) {
    Cpu saved = *c; /* DT_INIT follows the AAPCS64; the callback returns to guest code with its state intact */
    for (size_t i = 0; i < dkr_module_count; i++) {
        const DkrModule *m = &dkr_modules[i];
        if (!m->init) continue;
        char a[96];
        hos_log("boot: DT_INIT of %s at %s\n", m->name, rt_describe(m->init, a, sizeof a));
        rt_call(c, m->init);
        hos_log("boot: DT_INIT of %s returned\n", m->name);
    }
    for (int r = 19; r <= 30; r++) c->x[r] = saved.x[r];
    c->sp = saved.sp;
}

static void cb_returned(Cpu *c) { hos_stop(c, "the guest main thread returned to the host"); }
static void cb_thread_returned(Cpu *c) { (void)c; } /* a thread's entry returned: kernel.c ends the thread */

/* Ends the run after `seconds` with every thread's state: hangs and deadlocks become a report, not silence. */
static DWORD WINAPI watchdog(void *p) {
    Sleep((DWORD)(uintptr_t)p * 1000);
    hos_log("\nboot: watchdog after %u s, %llu SVCs; threads:\n", (unsigned)(uintptr_t)p, (unsigned long long)g_svc_count);
    kern_report();
    if (g_log) fclose(g_log);
    ExitProcess(4);
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    unsigned seconds = argc > 2 ? (unsigned)atoi(argv[2]) : 60;
    char default_romfs[4096];
    snprintf(default_romfs, sizeof default_romfs, "%s/../../user_files/romfs.bin", dir);
    const char *romfs_path = argc > 3 ? argv[3] : getenv("DKR_ROMFS");
    hos_fs_set_romfs(romfs_path ? romfs_path : default_romfs);
    char default_save_root[4096];
    snprintf(default_save_root, sizeof default_save_root, "%s/../../user_files/saves", dir);
    const char *save_root = getenv("DKR_SAVE_ROOT");
    hos_save_configure(save_root ? save_root : default_save_root, PROGRAM_ID);
    g_log = fopen("dkctf_boot.log", "w");
    if (rt_load_modules(dkr_modules, dkr_module_count, dir)) return 2;
    for (size_t i = 0; i < dkr_module_count; i++) {
        const DkrModule *m = &dkr_modules[i];
        region_add(m->base, m->text_hi - m->base, MS_CODE, 5);
        region_add(m->text_hi, m->base + m->size - m->text_hi, MS_CODEDATA, 3);
    }
    /* main thread: stack at the bottom of the stack region; thread local regions (0x200 bytes per thread) */
    uint64_t stack = dkr_main_thread_stack_size;
    if (rt_map_fixed(STACK_BASE, stack) || rt_map_fixed(TLR_BASE, TLR_REGION_SIZE)) {
        fprintf(stderr, "boot: cannot map the main thread's stack or the thread local regions\n");
        return 2;
    }
    region_add(STACK_BASE, stack, MS_STACK, 3);
    region_add(TLR_BASE, TLR_REGION_SIZE, MS_THREADLOCAL, 3);
    g_fixed_used = stack + TLR_REGION_SIZE;
    for (size_t i = 0; i < dkr_module_count; i++) g_fixed_used += dkr_modules[i].size;
    dkr_svc_hook = svc_handler;
#ifdef DKR_ENABLE_GFX
    dkr_nvn_register();
#endif

    Cpu *c;
    uint32_t main_handle = kern_init(&c, STACK_BASE + stack, TLR_BASE, 44, 0,
                                     rt_hostfn_register("thread returned", cb_thread_returned));
    c->x[0] = main_handle;
    c->x[1] = dkr_modules[0].base; /* rtld passes the address right after itself: the first module's base */
    c->x[2] = rt_hostfn_register("rtld flag callback", cb_flag);
    c->x[3] = rt_hostfn_register("rtld module initializers", cb_module_inits);
    c->x[30] = rt_hostfn_register("return to host", cb_returned);
    CreateThread(NULL, 0, watchdog, (void *)(uintptr_t)seconds, 0, NULL);
    char a[96], b[96];
    hos_log("boot: calling nn::init::Start at %s (nnMain at %s), main thread %#x, watchdog %u s\n",
            rt_describe(dkr_sym_init_start, a, sizeof a), rt_describe(dkr_sym_nn_main, b, sizeof b), main_handle,
            seconds);
    rt_call(c, dkr_sym_init_start);
    hos_stop(c, "nn::init::Start returned");
    return 0;
}
