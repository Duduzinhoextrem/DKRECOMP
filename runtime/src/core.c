/* DKRecomp runtime core: indirect dispatch, exclusive-monitor CAS, fences, trap handlers, fixed mappings. */
#include "dkr/rt.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

DkrSvcHook dkr_svc_hook = NULL;

/* ---- dispatch tables ------------------------------------------------------------------------------------- */
#define MAX_TABLES 32
static struct { const DkrFnEntry *t; size_t n; uint64_t lo, hi; } g_tables[MAX_TABLES];
static size_t g_ntables;

void rt_register_table(const DkrFnEntry *table, size_t count, uint64_t lo, uint64_t hi) {
    if (g_ntables == MAX_TABLES) {
        fprintf(stderr, "dkr: too many dispatch tables\n");
        abort();
    }
    g_tables[g_ntables].t = table;
    g_tables[g_ntables].n = count;
    g_tables[g_ntables].lo = lo;
    g_tables[g_ntables].hi = hi;
    g_ntables++;
}

/* ---- host functions with guest addresses ---------------------------------------------------------------- */
#define MAX_HOSTFN 1024
static struct { const char *name; GuestFn fn; } g_hostfn[MAX_HOSTFN];
static size_t g_nhostfn;

uint64_t rt_hostfn_register(const char *name, GuestFn fn) {
    if (g_nhostfn == MAX_HOSTFN) {
        fprintf(stderr, "dkr: too many host functions\n");
        abort();
    }
    g_hostfn[g_nhostfn].name = name;
    g_hostfn[g_nhostfn].fn = fn;
    return DKR_HOSTFN_BASE + 4 * g_nhostfn++;
}

const char *rt_hostfn_name(uint64_t addr) {
    uint64_t k = (addr - DKR_HOSTFN_BASE) / 4;
    return addr >= DKR_HOSTFN_BASE && k < g_nhostfn ? g_hostfn[k].name : NULL;
}

GuestFn rt_lookup(uint64_t addr) {
    for (size_t i = 0; i < g_ntables; i++) {
        if (addr < g_tables[i].lo || addr >= g_tables[i].hi) continue;
        const DkrFnEntry *t = g_tables[i].t;
        size_t lo = 0, hi = g_tables[i].n;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (t[mid].addr < addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo < g_tables[i].n && t[lo].addr == addr) return t[lo].fn;
    }
    if (addr >= DKR_HOSTFN_BASE && (addr - DKR_HOSTFN_BASE) % 4 == 0 && (addr - DKR_HOSTFN_BASE) / 4 < g_nhostfn)
        return g_hostfn[(addr - DKR_HOSTFN_BASE) / 4].fn;
    return NULL;
}

/* ---- module names for logs ---------------------------------------------------------------------------------- */
static const DkrModule *g_mods;
static size_t g_nmods;

void rt_set_modules(const DkrModule *mods, size_t count) {
    g_mods = mods;
    g_nmods = count;
}

const char *rt_describe(uint64_t addr, char *buf, size_t n) {
    for (size_t i = 0; i < g_nmods; i++)
        if (addr >= g_mods[i].base && addr < g_mods[i].base + g_mods[i].size) {
            snprintf(buf, n, "%s+%#llx", g_mods[i].name, (unsigned long long)(addr - g_mods[i].base));
            return buf;
        }
    const char *h = rt_hostfn_name(addr);
    if (h) snprintf(buf, n, "<host %s>", h);
    else snprintf(buf, n, "%#llx", (unsigned long long)addr);
    return buf;
}

void rt_dispatch(Cpu *c) {
    GuestFn f = rt_lookup(c->pc);
    if (!f) {
        char a[96], l[96];
        fprintf(stderr, "dkr: indirect branch to %s, which is not a lifted function (lr=%s)\n",
                rt_describe(c->pc, a, sizeof a), rt_describe(c->x[30], l, sizeof l));
        fflush(stderr);
        abort();
    }
    f(c);
}

void rt_call(Cpu *c, uint64_t addr) {
    c->pc = addr;
    rt_dispatch(c);
}

void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    if (dkr_svc_hook) {
        dkr_svc_hook(c, imm, pc);
        return;
    }
    fprintf(stderr, "dkr: SVC %#x at %#llx and no SVC handler is installed\n", imm, (unsigned long long)pc);
    abort();
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)c;
    fprintf(stderr, "dkr: BRK %#x at %#llx\n", imm, (unsigned long long)pc);
    abort();
}

void rt_unsupported(Cpu *c, uint64_t pc, uint32_t word) {
    (void)c;
    fprintf(stderr, "dkr: reached unsupported instruction %08x at %#llx\n", word, (unsigned long long)pc);
    abort();
}

int rt_cas(uint64_t addr, uint64_t expect, uint64_t value, int nbytes) {
#ifdef DKR_MSVC
    switch (nbytes) {
    case 1: return (uint8_t)_InterlockedCompareExchange8((volatile char *)(uintptr_t)addr, (char)value,
                                                          (char)expect) == (uint8_t)expect;
    case 2: return (uint16_t)_InterlockedCompareExchange16((volatile short *)(uintptr_t)addr, (short)value,
                                                            (short)expect) == (uint16_t)expect;
    case 4: return (uint32_t)_InterlockedCompareExchange((volatile long *)(uintptr_t)addr, (long)value,
                                                          (long)expect) == (uint32_t)expect;
    default: return (uint64_t)_InterlockedCompareExchange64((volatile long long *)(uintptr_t)addr,
                                                             (long long)value, (long long)expect) == expect;
    }
#else
    switch (nbytes) {
    case 1: { uint8_t e = (uint8_t)expect; return __atomic_compare_exchange_n((uint8_t *)(uintptr_t)addr, &e,
                  (uint8_t)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    case 2: { uint16_t e = (uint16_t)expect; return __atomic_compare_exchange_n((uint16_t *)(uintptr_t)addr, &e,
                  (uint16_t)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    case 4: { uint32_t e = (uint32_t)expect; return __atomic_compare_exchange_n((uint32_t *)(uintptr_t)addr, &e,
                  (uint32_t)value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    default: { uint64_t e = expect; return __atomic_compare_exchange_n((uint64_t *)(uintptr_t)addr, &e, value, 0,
                  __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    }
#endif
}

void rt_fence(void) {
#ifdef _WIN32
    MemoryBarrier();
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

uint64_t rt_cntfrq(void) { return 19200000ull; } /* Switch system counter: 19.2 MHz (switchbrew, SVC GetSystemTick) */

uint64_t rt_cntvct(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER n;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&n);
    /* scale host ticks to 19.2 MHz without overflowing: split into seconds and remainder */
    uint64_t sec = (uint64_t)n.QuadPart / (uint64_t)f.QuadPart, rem = (uint64_t)n.QuadPart % (uint64_t)f.QuadPart;
    return sec * 19200000ull + rem * 19200000ull / (uint64_t)f.QuadPart;
#else
    return 0;
#endif
}

int rt_map_fixed(uint64_t addr, uint64_t size) {
#ifdef _WIN32
    /* Reservations are made in 64 KiB granules. Each granule the range touches is reserved on its own (one
     * shared with an earlier mapping is already reserved), and because a commit cannot span two reservations,
     * the requested pages are committed granule by granule. */
    const uint64_t G = 0x10000, end = addr + size;
    for (uint64_t g = addr & ~(G - 1); g < end; g += G) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)g, &mi, sizeof mi)) return -1;
        if (mi.State == MEM_FREE && VirtualAlloc((void *)(uintptr_t)g, G, MEM_RESERVE, PAGE_NOACCESS) != (void *)(uintptr_t)g)
            return -1;
        uint64_t lo = g > addr ? g : addr, hi = g + G < end ? g + G : end;
        if (VirtualAlloc((void *)(uintptr_t)lo, (SIZE_T)(hi - lo), MEM_COMMIT, PAGE_READWRITE) != (void *)(uintptr_t)lo)
            return -1;
    }
    return 0;
#else
    (void)addr; (void)size;
    return -1;
#endif
}

unsigned long rt_last_os_error(void) {
#ifdef _WIN32
    return GetLastError();
#else
    return 0;
#endif
}

void rt_unmap(uint64_t addr) {
#ifdef _WIN32
    /* Only releases a range whose reservation started at `addr` (a 64 KiB granule); see rt_map_fixed. */
    VirtualFree((void *)(uintptr_t)addr, 0, MEM_RELEASE);
#else
    (void)addr;
#endif
}
