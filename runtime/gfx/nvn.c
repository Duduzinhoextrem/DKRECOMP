/* NVN entry-point bridge for the statically recompiled SDK.
 * The public proc name list is from the MIT TotK/MacNativeGames reference; see third_party/ and notices.
 * This bridge uses DKRecomp's actual Cpu and host-function table. Unimplemented procs stop by name. */
#include "dkr/nvn.h"
#include "dkr/gfx.h"
#include "nvn_internal.h"
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
    /* Legacy public entry points requested by the target's NVN 4.4 loader, removed from the newer list. */
    {"nvnDeviceApplyDeferredFinalizes", 0, 0},
    {"nvnQueueAcquireTexture", 0, 0},
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

static void device_builder_defaults(Cpu *c) {
    /* Verified against the target's original routine: exactly two 64-bit stores, not sizeof a newer builder. */
    memset((void *)(uintptr_t)c->x[0], 0, 16);
}

static void device_builder_flags(Cpu *c) {
    uint32_t flags = (uint32_t)c->x[1];
    memcpy((void *)(uintptr_t)c->x[0], &flags, sizeof flags);
}

/* ---- device ------------------------------------------------------------------------------------------------
 * The game embeds NVNdevice (0x3000 bytes) in NVNManaged::CDevice and keeps its own "initialised" byte at +0x3000
 * (main+0x4ec24), so host state stays well inside the reserved size. */
#define NVN_DEVICE_SIZE 0x3000
#define DEV_MAGIC 0x00564544u /* "DEV" */
typedef struct DkrNvnDevice {
    uint32_t magic, flags;
} DkrNvnDevice;
#define NVN_DEVICE_WINDOW_ORIGIN 0x1978 /* written by the original nvnDeviceSetWindowOriginMode (sdk+0x2def98) */
#define NVN_DEVICE_DEPTH_MODE 0x197c    /* written by the original nvnDeviceSetDepthMode (sdk+0x2def8c) */
_Static_assert(sizeof(DkrNvnDevice) <= NVN_DEVICE_SIZE, "device state must fit NVNdevice");

static void device_initialize(Cpu *c) { /* NVNboolean nvnDeviceInitialize(NVNdevice *, const NVNdeviceBuilder *) */
    DkrNvnDevice *d = (DkrNvnDevice *)(uintptr_t)c->x[0];
    uint32_t flags;
    memcpy(&flags, (const void *)(uintptr_t)c->x[1], sizeof flags);
    memset(d, 0, NVN_DEVICE_SIZE); /* also the original fields at +0x1978/+0x197c: lower-left origin, -w depth */
    d->magic = DEV_MAGIC;
    d->flags = flags;
    hos_log("nvn: DeviceInitialize %#llx flags %#x\n", (unsigned long long)c->x[0], flags);
    c->x[0] = 1;
}

static void device_finalize(Cpu *c) { ((DkrNvnDevice *)(uintptr_t)c->x[0])->magic = 0; }

/* NVNdeviceInfo values. 0/1 are the API version: NVNManaged::CDevice::Init (main+0x4eb98) requires major 53 and
 * minor >= 105, the NVN of SDK 4.4. The rest are Tegra X1 limits as used by the MIT TotK reference
 * (runtime/nvn_api_core.c devinfo, "nnheaders nvn_DeviceConstantsNX.h, measured on SMO"); the enum keeps its
 * order across NVN versions, newer entries only being appended. */
static const int g_devinfo[] = {
    [0x00] = 53, [0x01] = 105,
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
    [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
    [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
    [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 1, [0x33] = 8,
    [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
    [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 4096,
};

static void device_get_integer(Cpu *c) { /* void nvnDeviceGetInteger(const NVNdevice *, NVNdeviceInfo, int *) */
    uint32_t what = (uint32_t)c->x[1];
    if (what >= sizeof g_devinfo / sizeof *g_devinfo) hos_stop(c, "nvnDeviceGetInteger: property %#x is not known", what);
    int value = g_devinfo[what];
    memcpy((void *)(uintptr_t)c->x[2], &value, sizeof value);
    hos_log("nvn: DeviceGetInteger %#x -> %d\n", what, value);
}

/* NVNboolean nvnDeviceRegisterFastClear{Color,Colori,Colorui}(dev, const T *color, NVNformat), ...Depth(dev, float).
 * Fast-clear registration is a hint for the Tegra compression hardware. The game only prints "Failed to register
 * fast clear ..." on 0 and continues (NVNManaged::CDevice::Init, main+0x4ebe8/0x4ec18), so "not registered" is
 * the honest answer and keeps the ordinary clear path. */
static void device_register_fast_clear(Cpu *c) { c->x[0] = 0; }

static void ignore_call(Cpu *c) { (void)c; } /* FinalizeCommandHandle: nothing is held per handle on the host */

/* name -> host implementation (this file and nvn_obj.c); anything else stops by name (unsupported_proc) */
static const NvnImpl g_impls[] = {
    {"nvnDeviceGetProcAddress", device_get_proc_address},
    {"nvnDeviceBuilderSetDefaults", device_builder_defaults},
    {"nvnDeviceBuilderSetFlags", device_builder_flags},
    {"nvnDeviceInitialize", device_initialize},
    {"nvnDeviceFinalize", device_finalize},
    {"nvnDeviceGetInteger", device_get_integer},
    {"nvnDeviceRegisterFastClearColor", device_register_fast_clear},
    {"nvnDeviceRegisterFastClearColori", device_register_fast_clear},
    {"nvnDeviceRegisterFastClearColorui", device_register_fast_clear},
    {"nvnDeviceRegisterFastClearDepth", device_register_fast_clear},
    {"nvnDeviceFinalizeCommandHandle", ignore_call},
};

static GuestFn implementation_of(const char *name) {
    for (size_t i = 0; i < sizeof g_impls / sizeof *g_impls; i++)
        if (!strcmp(g_impls[i].name, name)) return g_impls[i].fn;
    for (size_t i = 0; i < dkr_nvn_obj_impl_count; i++)
        if (!strcmp(dkr_nvn_obj_impls[i].name, name)) return dkr_nvn_obj_impls[i].fn;
    GuestFn family = dkr_nvn_obj_fallback(name);
    return family ? family : unsupported_proc;
}

/* Categories served by the original driver routine when the analysis found it pure. Builders and state objects
 * are CPU-side structs by NVN's design; handle encoders compute from their arguments; the device mode accessors
 * read and write NVNdevice+0x1978/0x197c (window origin, depth mode), which DeviceInitialize zeroes. Objects whose
 * Initialize talks to the GPU (pools, queues, textures, command buffers, windows, syncs) stay on the host, getters
 * included, so their layout is ours alone. */
static int original_allowed(const char *name) {
    static const char *const prefixes[] = {
        "nvnBlendState", "nvnChannelMaskState", "nvnColorState", "nvnDepthStencilState", "nvnMultisampleState",
        "nvnPolygonState", "nvnVertexAttribState", "nvnVertexStreamState", "nvnTextureView",
        /* descriptor pools {memory pool +0, offset +8, count +0x10}: getters only; the host initializes them and
         * keeps the registered descriptors */
        "nvnTexturePoolGet", "nvnSamplerPoolGet",
    };
    static const char *const exact[] = {
        "nvnDeviceGetImageHandle", "nvnDeviceGetTextureHandle", "nvnDeviceGetTexelFetchHandle",
        "nvnDeviceGetSeparateTextureHandle", "nvnDeviceGetSeparateSamplerHandle", "nvnDeviceInstallDebugCallback",
        "nvnDeviceSetWindowOriginMode", "nvnDeviceGetWindowOriginMode", "nvnDeviceSetDepthMode",
        "nvnDeviceGetDepthMode",
    };
    if (strstr(name, "Builder") && !strstr(name, "RegisterSamplerBuilder")) return 1;
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++)
        if (!strncmp(name, prefixes[i], strlen(prefixes[i]))) return 1;
    for (size_t i = 0; i < sizeof exact / sizeof *exact; i++)
        if (!strcmp(name, exact[i])) return 1;
    size_t n = strlen(name);
    return n > 13 && !strcmp(name + n - 13, "SetDebugLabel");
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

static const DkrNvnOriginal *g_originals;
static size_t g_original_count;

uint64_t dkr_nvn_original(const char *name) {
    for (size_t i = 0; i < g_original_count; i++)
        if (!strcmp(g_originals[i].name, name)) return g_originals[i].address;
    return 0;
}

static uint64_t original_of(const char *name) { return original_allowed(name) ? dkr_nvn_original(name) : 0; }

void dkr_nvn_register(const DkrNvnOriginal *originals, size_t original_count) {
    if (g_registered) return;
    g_originals = originals;
    g_original_count = originals ? original_count : 0;
    size_t count = sizeof g_procs / sizeof *g_procs, from_driver = 0;
    qsort(g_procs, count, sizeof *g_procs, compare_procs);
    for (size_t i = 0; i < count; i++) {
        if (i && strcmp(g_procs[i - 1].name, g_procs[i].name) >= 0) {
            fprintf(stderr, "nvn: proc names must be sorted and unique\n");
            abort();
        }
        uint64_t original = original_of(g_procs[i].name);
        if (original && !rt_lookup(original)) {
            fprintf(stderr, "nvn: original %s at %#llx is not a lifted function\n", g_procs[i].name,
                    (unsigned long long)original);
            abort();
        }
        if (original) from_driver++;
        g_procs[i].address = original ? original : rt_hostfn_register(g_procs[i].name, implementation_of(g_procs[i].name));
    }
    g_registered = 1;
    atexit(cleanup_graphics);
    hos_log("nvn: registered %zu proc addresses, %zu served by original pure driver routines; unsupported "
            "operations stop when called\n", count, from_driver);
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
