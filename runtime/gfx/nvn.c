/* NVN entry-point bridge for the statically recompiled SDK.
 * The public proc name list is from the MIT TotK/MacNativeGames reference; see third_party/ and notices.
 * This bridge uses DKRecomp's actual Cpu and host-function table. Unimplemented procs stop by name. */
#include "dkr/nvn.h"
#include "dkr/gfx.h"
#include "../src/hos.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct NvnProc {
    const char *name;
    uint64_t address;
    LONG queried;
} NvnProc;

#define NVN_PROC(name) {#name, 0, 0},
static NvnProc g_procs[] = {
#include "third_party/nvn_procs.inc"
};
#undef NVN_PROC

static DkrGfx *g_graphics;
static uint64_t g_work_memory, g_work_size, g_devtools_memory, g_devtools_size;
static int g_registered;

static void unsupported_proc(Cpu *c) {
    const char *name = rt_hostfn_name(c->pc);
    hos_stop(c, "NVN %s is not implemented", name ? name : "unknown proc");
}

uint64_t dkr_nvn_proc_address(const char *name) {
    if (!name) return 0;
    size_t lo = 0, hi = sizeof g_procs / sizeof *g_procs;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int order = strcmp(g_procs[mid].name, name);
        if (order < 0) lo = mid + 1;
        else hi = mid;
    }
    if (lo == sizeof g_procs / sizeof *g_procs || strcmp(g_procs[lo].name, name)) {
        hos_log("nvn: unknown proc '%s', returning NULL\n", name);
        return 0;
    }
    NvnProc *proc = &g_procs[lo];
    if (!InterlockedExchange(&proc->queried, 1)) hos_log("nvn: proc %s -> %#llx\n", proc->name, (unsigned long long)proc->address);
    return proc->address;
}

static void device_get_proc_address(Cpu *c) {
    c->x[0] = dkr_nvn_proc_address((const char *)(uintptr_t)c->x[1]);
}

static void cleanup_graphics(void) {
    if (!g_graphics) return;
    uint32_t errors = dkr_gfx_destroy(g_graphics);
    g_graphics = NULL;
    if (errors) fprintf(stderr, "nvn: %u Vulkan validation errors during shutdown\n", errors);
}

static int compare_procs(const void *left, const void *right) {
    return strcmp(((const NvnProc *)left)->name, ((const NvnProc *)right)->name);
}

void dkr_nvn_register(void) {
    if (g_registered) return;
    size_t count = sizeof g_procs / sizeof *g_procs;
    qsort(g_procs, count, sizeof *g_procs, compare_procs);
    for (size_t i = 0; i < count; i++) {
        if (i && strcmp(g_procs[i - 1].name, g_procs[i].name) >= 0) {
            fprintf(stderr, "nvn: proc names must be sorted and unique\n");
            abort();
        }
        GuestFn implementation = !strcmp(g_procs[i].name, "nvnDeviceGetProcAddress") ? device_get_proc_address : unsupported_proc;
        g_procs[i].address = rt_hostfn_register(g_procs[i].name, implementation);
    }
    g_registered = 1;
    atexit(cleanup_graphics);
    hos_log("nvn: registered %zu proc addresses; unsupported operations stop when called\n", count);
}

void dkr_nvn_graphics_init(Cpu *c) {
    if (!g_registered || g_graphics) hos_stop(c, "nv::InitializeGraphics called out of sequence");
    if (!c->x[0] || !c->x[1]) hos_stop(c, "nv::InitializeGraphics needs guest work memory");
    g_work_memory = c->x[0]; g_work_size = c->x[1];
    const char *hidden = getenv("DKR_GFX_HIDDEN");
    DkrGfxConfig config = {1280, 720, "DKRecomp", !(hidden && *hidden && strcmp(hidden, "0")), -1};
    if (dkr_gfx_create(&g_graphics, &config)) hos_stop(c, "Vulkan initialization failed: %s", dkr_gfx_error(g_graphics));
    hos_log("nvn: host Vulkan device ready, guest work memory %#llx +%#llx; no game frame submitted\n",
            (unsigned long long)g_work_memory, (unsigned long long)g_work_size);
}

void dkr_nvn_graphics_finalize(Cpu *c) {
    (void)c;
    cleanup_graphics();
    g_work_memory = g_work_size = 0;
}

void dkr_nvn_devtools_init(Cpu *c) {
    if (!c->x[0] || !c->x[1]) hos_stop(c, "nv::InitializeGraphicsDevtools needs guest work memory");
    g_devtools_memory = c->x[0]; g_devtools_size = c->x[1];
    hos_log("nvn: devtools work memory %#llx +%#llx recorded\n", (unsigned long long)g_devtools_memory,
            (unsigned long long)g_devtools_size);
}

void dkr_nvn_devtools_finalize(Cpu *c) { (void)c; g_devtools_memory = g_devtools_size = 0; }

void dkr_nvn_bootstrap(Cpu *c) {
    c->x[0] = dkr_nvn_proc_address((const char *)(uintptr_t)c->x[0]);
}
