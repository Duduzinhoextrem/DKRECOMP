/* NVN objects the driver keeps on the GPU side: syncs, memory pools, buffers, events and queues.
 *
 * Builders are the original driver routines (nvn.c original_allowed), so Initialize reads them with the layouts of
 * the target's own setters, measured with dkrecomp.nvnmap (sdk offsets below). Objects are stored in place in the
 * memory the game provides, within the NVN 53.105 reserved sizes (h1k421 nvn.h in the MIT TotK reference, from
 * DWARF of a 53.105 title; NVNsync's 0x40 also matches the stride of the game's sync array at main+0x7b9ac).
 *
 * GPU model so far: no back end consumes work yet, so a fence covers "everything submitted so far", which counts
 * as complete at once. Nothing is drawn by this file. Semantics follow the reference's nvn_api_core.c. */
#include "nvn_internal.h"
#include "nvn_tex.h"
#include "../src/hos.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FITS(type, size) _Static_assert(sizeof(type) <= (size), #type " must fit the NVN reserved size")
#define PTR(v) ((void *)(uintptr_t)(v))

static uint64_t rd64(uint64_t addr) { uint64_t v; memcpy(&v, PTR(addr), 8); return v; }
static uint32_t rd32(uint64_t addr) { uint32_t v; memcpy(&v, PTR(addr), 4); return v; }

/* ---- fences ---------------------------------------------------------------------------------------------------- */
static volatile LONG64 g_submits; /* QueueSubmitCommands calls so far; each is complete when it returns */

#define SYNC_MAGIC MAGIC('S', 'Y', 'N', 'C')
typedef struct { uint32_t magic, fenced; uint64_t fence; } Sync;
FITS(Sync, 0x40);

static Sync *sync_of(Cpu *c, uint64_t addr) {
    Sync *s = PTR(addr);
    if (!addr || s->magic != SYNC_MAGIC) hos_stop(c, "nvn: %#llx is not an initialized NVNsync", (unsigned long long)addr);
    return s;
}

static void sync_initialize(Cpu *c) { /* NVNboolean (NVNsync *, NVNdevice *) */
    *(Sync *)PTR(c->x[0]) = (Sync){SYNC_MAGIC, 0, 0};
    c->x[0] = 1;
}

static void sync_finalize(Cpu *c) { ((Sync *)PTR(c->x[0]))->magic = 0; }

static void queue_fence_sync(Cpu *c) { /* void (NVNqueue *, NVNsync *, NVNsyncCondition, int flags) */
    Sync *s = sync_of(c, c->x[1]);
    s->fenced = 1;
    s->fence = (uint64_t)g_submits;
}

/* NVNsyncWaitResult (const NVNsync *, uint64_t timeout_ns). A sync never fenced times out at once, as the reference
 * does: no submitted work can signal it. */
static void sync_wait(Cpu *c) { c->x[0] = sync_of(c, c->x[0])->fenced ? 0 /* ALREADY_SIGNALED */ : 2 /* TIMEOUT */; }

static void queue_wait_sync(Cpu *c) { sync_of(c, c->x[1]); c->x[0] = 1; } /* NVNboolean (NVNqueue *, const NVNsync *) */

/* ---- memory pools ---------------------------------------------------------------------------------------------- */
#define POOL_MAGIC MAGIC('P', 'O', 'O', 'L')
#define POOL_CPU_NO_ACCESS 0x1u
#define POOL_PHYSICAL 0x100u
#define POOL_VIRTUAL 0x200u
typedef struct { uint32_t magic, flags; uint64_t mem, size; } Pool;
FITS(Pool, 0x100);
/* NVNmemoryPoolBuilder as the original setters write it (sdk+0x2e0d48 SetDevice, +0x2e0d70 SetStorage,
 * +0x2e0db8 SetFlags): device +0, storage +8, size +0x10, flags +0x18. */
enum { PB_STORAGE = 0x08, PB_SIZE = 0x10, PB_FLAGS = 0x18 };

static Pool *pool_of(Cpu *c, uint64_t addr) {
    Pool *p = PTR(addr);
    if (!addr || p->magic != POOL_MAGIC) hos_stop(c, "nvn: %#llx is not an initialized NVNmemoryPool", (unsigned long long)addr);
    return p;
}

static void pool_initialize(Cpu *c) { /* NVNboolean (NVNmemoryPool *, const NVNmemoryPoolBuilder *) */
    uint64_t b = c->x[1];
    Pool p = {POOL_MAGIC, rd32(b + PB_FLAGS), rd64(b + PB_STORAGE), rd64(b + PB_SIZE)};
    if (p.flags & (POOL_PHYSICAL | POOL_VIRTUAL))
        hos_stop(c, "nvn: physical/virtual memory pools (flags %#x) are not implemented", p.flags);
    if (!p.mem || !p.size) hos_stop(c, "nvn: memory pool without storage (mem %#llx size %#llx)",
                                   (unsigned long long)p.mem, (unsigned long long)p.size);
    if (p.mem & 0xfff || p.size & 0xfff)
        hos_stop(c, "nvn: memory pool storage %#llx+%#llx is not 4 KiB aligned", (unsigned long long)p.mem,
                 (unsigned long long)p.size);
    *(Pool *)PTR(c->x[0]) = p;
    hos_log("nvn: MemoryPool %#llx storage %#llx+%#llx flags %#x\n", (unsigned long long)c->x[0],
            (unsigned long long)p.mem, (unsigned long long)p.size, p.flags);
    c->x[0] = 1;
}

static void pool_finalize(Cpu *c) { ((Pool *)PTR(c->x[0]))->magic = 0; }
static void pool_map(Cpu *c) { Pool *p = pool_of(c, c->x[0]); c->x[0] = p->flags & POOL_CPU_NO_ACCESS ? 0 : p->mem; }
/* GPU addresses equal guest addresses of the storage until a back end maps pools somewhere else. */
static void pool_buffer_address(Cpu *c) { c->x[0] = pool_of(c, c->x[0])->mem; }
static void pool_get_flags(Cpu *c) { c->x[0] = pool_of(c, c->x[0])->flags; }
static void pool_get_size(Cpu *c) { c->x[0] = pool_of(c, c->x[0])->size; }

static void pool_mapped_range(Cpu *c) { /* Flush/InvalidateMappedRange (pool, ptrdiff_t off, size_t size) */
    Pool *p = pool_of(c, c->x[0]);
    uint64_t off = c->x[1], size = c->x[2];
    if (off > p->size || size > p->size - off)
        hos_stop(c, "nvn: %s range %#llx+%#llx outside the pool (%#llx bytes)", rt_hostfn_name(c->pc),
                 (unsigned long long)off, (unsigned long long)size, (unsigned long long)p->size);
}

/* ---- buffers: a range of a pool -------------------------------------------------------------------------------- */
#define BUF_MAGIC MAGIC('B', 'U', 'F', 0)
typedef struct { uint32_t magic, pad; uint64_t pool, off, size; } Buffer;
FITS(Buffer, 0x30);
/* NVNbufferBuilder (sdk+0x2d58dc SetDevice, +0x2d58f0 SetStorage): device +0, pool +8, offset +0x10, size +0x18. */
enum { BB_POOL = 0x08, BB_OFFSET = 0x10, BB_SIZE = 0x18 };

static Buffer *buffer_of(Cpu *c, uint64_t addr) {
    Buffer *b = PTR(addr);
    if (!addr || b->magic != BUF_MAGIC) hos_stop(c, "nvn: %#llx is not an initialized NVNbuffer", (unsigned long long)addr);
    return b;
}

static void buffer_initialize(Cpu *c) { /* NVNboolean (NVNbuffer *, const NVNbufferBuilder *) */
    uint64_t b = c->x[1];
    Buffer o = {BUF_MAGIC, 0, rd64(b + BB_POOL), rd64(b + BB_OFFSET), rd64(b + BB_SIZE)};
    Pool *p = pool_of(c, o.pool);
    if (o.off > p->size || o.size > p->size - o.off)
        hos_stop(c, "nvn: buffer %#llx+%#llx outside its pool (%#llx bytes)", (unsigned long long)o.off,
                 (unsigned long long)o.size, (unsigned long long)p->size);
    *(Buffer *)PTR(c->x[0]) = o;
    c->x[0] = 1;
}

static void buffer_finalize(Cpu *c) { ((Buffer *)PTR(c->x[0]))->magic = 0; }
static uint64_t buffer_address(Cpu *c, Buffer *b) { return pool_of(c, b->pool)->mem + b->off; }
static void buffer_get_address(Cpu *c) { c->x[0] = buffer_address(c, buffer_of(c, c->x[0])); }

static void buffer_map(Cpu *c) {
    Buffer *b = buffer_of(c, c->x[0]);
    c->x[0] = pool_of(c, b->pool)->flags & POOL_CPU_NO_ACCESS ? 0 : buffer_address(c, b);
}

static void buffer_get_pool(Cpu *c) { c->x[0] = buffer_of(c, c->x[0])->pool; }
static void buffer_get_offset(Cpu *c) { c->x[0] = buffer_of(c, c->x[0])->off; }
static void buffer_get_size(Cpu *c) { c->x[0] = buffer_of(c, c->x[0])->size; }

static void buffer_mapped_range(Cpu *c) { /* Flush/InvalidateMappedRange (buffer, ptrdiff_t off, size_t size) */
    Buffer *b = buffer_of(c, c->x[0]);
    uint64_t off = c->x[1], size = c->x[2];
    if (off > b->size || size > b->size - off)
        hos_stop(c, "nvn: %s range %#llx+%#llx outside the buffer (%#llx bytes)", rt_hostfn_name(c->pc),
                 (unsigned long long)off, (unsigned long long)size, (unsigned long long)b->size);
}

/* ---- events: a 32-bit value in pool storage --------------------------------------------------------------------- */
#define EVENT_MAGIC MAGIC('E', 'V', 'N', 'T')
typedef struct { uint32_t magic, pad; uint64_t value; } Event;
FITS(Event, 0x40);
/* NVNeventBuilder (sdk+0x2e07e8 SetStorage): pool +0, offset +8. */

static Event *event_of(Cpu *c, uint64_t addr) {
    Event *e = PTR(addr);
    if (!addr || e->magic != EVENT_MAGIC) hos_stop(c, "nvn: %#llx is not an initialized NVNevent", (unsigned long long)addr);
    return e;
}

static void event_initialize(Cpu *c) { /* NVNboolean (NVNevent *, const NVNeventBuilder *) */
    Pool *p = pool_of(c, rd64(c->x[1]));
    uint64_t off = rd64(c->x[1] + 8);
    if (off > p->size - 4 || off & 3) hos_stop(c, "nvn: event storage offset %#llx invalid", (unsigned long long)off);
    *(Event *)PTR(c->x[0]) = (Event){EVENT_MAGIC, 0, p->mem + off};
    c->x[0] = 1;
}

static void event_finalize(Cpu *c) { ((Event *)PTR(c->x[0]))->magic = 0; }
static void event_get_value(Cpu *c) { c->x[0] = *(volatile uint32_t *)PTR(event_of(c, c->x[0])->value); }

static void event_signal(Cpu *c) { /* void (NVNevent *, NVNeventSignalMode, uint32_t value) */
    volatile uint32_t *v = PTR(event_of(c, c->x[0])->value);
    if ((uint32_t)c->x[1] == 1 /* ADD */) *v += (uint32_t)c->x[2];
    else *v = (uint32_t)c->x[2];
}

/* ---- queues --------------------------------------------------------------------------------------------------- */
#define QUEUE_MAGIC MAGIC('Q', 'U', 'E', 'U')
typedef struct { uint32_t magic, flags; uint64_t device, memory, memory_size; } Queue;
FITS(Queue, 0x2000);
/* NVNqueueBuilder (sdk+0x2e4e04..0x2e4e68): device +0, flags +8, memory size +0x1c (u32), memory +0x28. */
enum { QB_FLAGS = 0x08, QB_MEMORY_SIZE = 0x1c, QB_MEMORY = 0x28 };

static Queue *queue_of(Cpu *c, uint64_t addr) {
    Queue *q = PTR(addr);
    if (!addr || q->magic != QUEUE_MAGIC) hos_stop(c, "nvn: %#llx is not an initialized NVNqueue", (unsigned long long)addr);
    return q;
}

static void queue_initialize(Cpu *c) { /* NVNboolean (NVNqueue *, const NVNqueueBuilder *) */
    uint64_t b = c->x[1];
    Queue q = {QUEUE_MAGIC, rd32(b + QB_FLAGS), rd64(b), rd64(b + QB_MEMORY), rd32(b + QB_MEMORY_SIZE)};
    *(Queue *)PTR(c->x[0]) = q;
    hos_log("nvn: Queue %#llx flags %#x memory %#llx+%#llx\n", (unsigned long long)c->x[0], q.flags,
            (unsigned long long)q.memory, (unsigned long long)q.memory_size);
    c->x[0] = 1;
}

static void queue_finalize(Cpu *c) { ((Queue *)PTR(c->x[0]))->magic = 0; }
static void queue_flush(Cpu *c) { queue_of(c, c->x[0]); } /* submitted work is already complete */
static void queue_finish(Cpu *c) { queue_of(c, c->x[0]); }

/* NVNqueueGetErrorResult (NVNqueue *, NVNqueueErrorInfo *). The info layout of 53.105 is not known and there is
 * no error to describe, so it is left untouched. */
static void queue_get_error(Cpu *c) {
    queue_of(c, c->x[0]);
    c->x[0] = 0; /* GPU_NO_ERROR */
}

/* ---- textures ------------------------------------------------------------------------------------------------- */
/* NVNtextureBuilder as the original setters write it (sdk+0x2e851c..0x2e85f0): device +0, pool +8, offset +0x10,
 * flags +0x18, target +0x1c, width/height/depth +0x20/+0x24/+0x28, levels +0x2c, format +0x30, samples +0x34,
 * swizzle +0x38..+0x44, depth-stencil mode +0x48, packaged data +0x50, packaged layout +0x58 (u16), stride +0x5c,
 * GL name +0x60. */
#define TB_SIZE 0x64
enum { TB_POOL = 0x08, TB_OFFSET = 0x10, TB_FLAGS = 0x18, TB_TARGET = 0x1c, TB_WIDTH = 0x20, TB_HEIGHT = 0x24,
       TB_DEPTH = 0x28, TB_LEVELS = 0x2c, TB_FORMAT = 0x30, TB_SAMPLES = 0x34, TB_SWIZZLE = 0x38, TB_DSMODE = 0x48,
       TB_STRIDE = 0x5c };
#define TEX_MAGIC MAGIC('T', 'E', 'X', 0)
/* In place: a copy of the builder (device slot included) plus the storage size computed by the original driver.
 * The original texture getters read up to +0xa8 (nvnTextureFinalize), so NVNtexture is at least 0xac bytes. */
typedef struct { uint32_t magic, serial; uint64_t storage_size; uint8_t builder[TB_SIZE]; } Texture;
FITS(Texture, 0xac);
static volatile LONG g_texture_serial;

static Texture *texture_of(Cpu *c, uint64_t addr) {
    Texture *t = PTR(addr);
    if (!addr || t->magic != TEX_MAGIC) hos_stop(c, "nvn: %#llx is not an initialized NVNtexture", (unsigned long long)addr);
    return t;
}

static uint32_t tex32(const Texture *t, int off) { uint32_t v; memcpy(&v, t->builder + off, 4); return v; }
static uint64_t tex64(const Texture *t, int off) { uint64_t v; memcpy(&v, t->builder + off, 8); return v; }

/* Layout from builder bytes. The packaged layout halfword (+0x58, sdk+0x2e88cc): bit 0 set, block height log2 in
 * bits 4..7, block depth log2 in bits 8..11. */
static void layout_of(Cpu *c, const uint8_t *b, NvnTexLayout *out) {
    uint32_t v[9];
    static const int offs[9] = {TB_FLAGS, TB_TARGET, TB_WIDTH, TB_HEIGHT, TB_DEPTH, TB_LEVELS, TB_FORMAT, TB_SAMPLES, TB_STRIDE};
    for (int i = 0; i < 9; i++) memcpy(&v[i], b + offs[i], 4);
    uint16_t pkg;
    memcpy(&pkg, b + 0x58, 2);
    NvnTexDesc d = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], pkg & 1, (pkg >> 4) & 0xf, (pkg >> 8) & 0xf};
    nvn_tex_layout(&d, out);
    if (!out->bpb) hos_stop(c, "nvn: texture format %#x has no known block size", d.format);
}

static void builder_storage_size(Cpu *c) { NvnTexLayout l; layout_of(c, PTR(c->x[0]), &l); c->x[0] = l.size; }
static void builder_storage_alignment(Cpu *c) { NvnTexLayout l; layout_of(c, PTR(c->x[0]), &l); c->x[0] = l.align; }

/* Storage classes are compatibility identifiers between textures sharing memory. Every texture here is decoded
 * from byte-addressed pool storage by its own description, so class 1 names that convention and buffer textures
 * use 0 (the reference's choice, nvn_api_obj.c storage_class); it is not a Tegra memory kind. */
static uint64_t storage_class(const uint8_t *b) { uint32_t target; memcpy(&target, b + TB_TARGET, 4); return target == 10 ? 0 : 1; }
static void builder_storage_class(Cpu *c) { c->x[0] = storage_class(PTR(c->x[0])); }

static void texture_initialize(Cpu *c) { /* NVNboolean (NVNtexture *, const NVNtextureBuilder *) */
    Texture t = {TEX_MAGIC, (uint32_t)InterlockedIncrement(&g_texture_serial), 0, {0}};
    memcpy(t.builder, PTR(c->x[1]), TB_SIZE);
    NvnTexLayout l;
    layout_of(c, t.builder, &l);
    t.storage_size = l.size;
    uint64_t pool = tex64(&t, TB_POOL), off = tex64(&t, TB_OFFSET);
    if (pool) {
        Pool *p = pool_of(c, pool);
        if (off > p->size || t.storage_size > p->size - off)
            hos_stop(c, "nvn: texture storage %#llx+%#llx outside its pool (%#llx bytes)", (unsigned long long)off,
                     (unsigned long long)t.storage_size, (unsigned long long)p->size);
    }
    memcpy(PTR(c->x[0]), &t, sizeof t);
    hos_log("nvn: Texture #%u %ux%ux%u levels %u format %#x target %u flags %#x storage %#llx+%#llx\n", t.serial,
            tex32(&t, TB_WIDTH), tex32(&t, TB_HEIGHT), tex32(&t, TB_DEPTH), tex32(&t, TB_LEVELS),
            tex32(&t, TB_FORMAT), tex32(&t, TB_TARGET), tex32(&t, TB_FLAGS), (unsigned long long)off,
            (unsigned long long)t.storage_size);
    c->x[0] = 1;
}

static void texture_finalize(Cpu *c) { ((Texture *)PTR(c->x[0]))->magic = 0; }
static void texture_get_width(Cpu *c) { c->x[0] = tex32(texture_of(c, c->x[0]), TB_WIDTH); }
static void texture_get_height(Cpu *c) { c->x[0] = tex32(texture_of(c, c->x[0]), TB_HEIGHT); }
static void texture_get_depth(Cpu *c) { c->x[0] = tex32(texture_of(c, c->x[0]), TB_DEPTH); }
static void texture_get_levels(Cpu *c) { c->x[0] = tex32(texture_of(c, c->x[0]), TB_LEVELS); }
static void texture_get_format(Cpu *c) { c->x[0] = tex32(texture_of(c, c->x[0]), TB_FORMAT); }
static void texture_get_target(Cpu *c) { c->x[0] = tex32(texture_of(c, c->x[0]), TB_TARGET); }
static void texture_get_flags(Cpu *c) { c->x[0] = tex32(texture_of(c, c->x[0]), TB_FLAGS); }
static void texture_get_samples(Cpu *c) { c->x[0] = tex32(texture_of(c, c->x[0]), TB_SAMPLES); }
static void texture_get_stride(Cpu *c) { c->x[0] = tex32(texture_of(c, c->x[0]), TB_STRIDE); }
static void texture_get_pool(Cpu *c) { c->x[0] = tex64(texture_of(c, c->x[0]), TB_POOL); }
static void texture_get_offset(Cpu *c) { c->x[0] = tex64(texture_of(c, c->x[0]), TB_OFFSET); }
static void texture_get_storage_size(Cpu *c) { c->x[0] = texture_of(c, c->x[0])->storage_size; }
static void texture_get_ds_mode(Cpu *c) { c->x[0] = tex32(texture_of(c, c->x[0]), TB_DSMODE); }
static void texture_get_storage_class(Cpu *c) { c->x[0] = storage_class(texture_of(c, c->x[0])->builder); }

static void texture_get_swizzle(Cpu *c) { /* void (const NVNtexture *, NVNtextureSwizzle *r, *g, *b, *a) */
    Texture *t = texture_of(c, c->x[0]);
    for (int i = 0; i < 4; i++) {
        uint32_t v = tex32(t, TB_SWIZZLE + 4 * i);
        memcpy(PTR(c->x[1 + i]), &v, 4);
    }
}

/* ---- samplers -------------------------------------------------------------------------------------------------- */
/* NVNsamplerBuilder (sdk+0x2e6fa4..0x2e7048): device +0, min/mag filter +8/+0xc, wrap s/t/r +0x10/+0x14/+0x18,
 * lod clamp +0x1c/+0x20, lod bias +0x24, compare mode/func +0x28/+0x2c, border color +0x30, max anisotropy +0x40,
 * reduction filter +0x44. The original sampler getters read up to +0x4c, so NVNsampler is at least 0x50 bytes. */
#define SB_SIZE 0x48
#define SAMP_MAGIC MAGIC('S', 'M', 'P', 0)
typedef struct { uint32_t magic, pad; uint8_t builder[SB_SIZE]; } Sampler;
FITS(Sampler, 0x50);

static void sampler_initialize(Cpu *c) { /* NVNboolean (NVNsampler *, const NVNsamplerBuilder *) */
    Sampler s = {SAMP_MAGIC, 0, {0}};
    memcpy(s.builder, PTR(c->x[1]), SB_SIZE);
    memcpy(PTR(c->x[0]), &s, sizeof s);
    c->x[0] = 1;
}

static void sampler_finalize(Cpu *c) { ((Sampler *)PTR(c->x[0]))->magic = 0; }

/* ---- descriptor pools ----------------------------------------------------------------------------------------- */
/* In place, the original layout {memory pool +0, offset +8, count +0x10} that the original getters read. The
 * registered descriptors live in a host table keyed by the descriptor memory, which is what command buffers bind. */
typedef struct { uint64_t pool, offset; uint32_t count, pad; } DescPool;
FITS(DescPool, 0x18);
typedef struct { uint8_t texture[sizeof(Texture)], view[0x24]; } TexDesc; /* view: NVNtextureView, 0x24 bytes */
typedef struct { uint8_t sampler[sizeof(Sampler)]; } SampDesc;
typedef struct { uint64_t memory; uint32_t count, kind; void *entries; } DescTable;
static DescTable g_tables[64];
static SRWLOCK g_tables_lock = SRWLOCK_INIT;

static void desc_pool_initialize(Cpu *c, int kind) { /* NVNboolean (pool, const NVNmemoryPool *, ptrdiff_t, int n) */
    Pool *p = pool_of(c, c->x[1]);
    uint64_t off = c->x[2];
    int n = (int)c->x[3];
    size_t entry = kind ? sizeof(SampDesc) : sizeof(TexDesc), desc = 32; /* Maxwell TIC/TSC entries: 32 bytes */
    if (n <= 0 || off > p->size || (uint64_t)n * desc > p->size - off)
        hos_stop(c, "nvn: descriptor pool of %d entries at %#llx outside its memory pool", n, (unsigned long long)off);
    *(DescPool *)PTR(c->x[0]) = (DescPool){c->x[1], off, (uint32_t)n, 0};
    void *entries = calloc((size_t)n, entry);
    if (!entries) hos_stop(c, "nvn: out of host memory for %d descriptors", n);
    AcquireSRWLockExclusive(&g_tables_lock);
    size_t i = 0;
    while (i < sizeof g_tables / sizeof *g_tables && g_tables[i].memory && g_tables[i].memory != p->mem + off) i++;
    if (i == sizeof g_tables / sizeof *g_tables) hos_stop(c, "nvn: more than 64 descriptor pools");
    free(g_tables[i].entries);
    g_tables[i] = (DescTable){p->mem + off, (uint32_t)n, (uint32_t)kind, entries};
    ReleaseSRWLockExclusive(&g_tables_lock);
    hos_log("nvn: %s pool %#llx: %d descriptors at %#llx\n", kind ? "Sampler" : "Texture",
            (unsigned long long)c->x[0], n, (unsigned long long)(p->mem + off));
    c->x[0] = 1;
}

static void texture_pool_initialize(Cpu *c) { desc_pool_initialize(c, 0); }
static void sampler_pool_initialize(Cpu *c) { desc_pool_initialize(c, 1); }

static void *desc_entry(Cpu *c, uint64_t pool_obj, int kind, int id) {
    DescPool *d = PTR(pool_obj);
    uint64_t memory = pool_of(c, d->pool)->mem + d->offset;
    void *e = NULL;
    AcquireSRWLockShared(&g_tables_lock);
    for (size_t i = 0; i < sizeof g_tables / sizeof *g_tables; i++)
        if (g_tables[i].memory == memory && g_tables[i].kind == (uint32_t)kind && id >= 0 && (uint32_t)id < g_tables[i].count)
            e = (uint8_t *)g_tables[i].entries + (size_t)id * (kind ? sizeof(SampDesc) : sizeof(TexDesc));
    ReleaseSRWLockShared(&g_tables_lock);
    if (!e) hos_stop(c, "nvn: descriptor %d is outside the %s pool at %#llx", id, kind ? "sampler" : "texture",
                     (unsigned long long)memory);
    return e;
}

static void texture_pool_register(Cpu *c) { /* RegisterTexture/RegisterImage (pool, int id, texture, view) */
    TexDesc *e = desc_entry(c, c->x[0], 0, (int)c->x[1]);
    memcpy(e->texture, texture_of(c, c->x[2]), sizeof(Texture));
    if (c->x[3]) memcpy(e->view, PTR(c->x[3]), sizeof e->view);
    else memset(e->view, 0, sizeof e->view);
}

static void sampler_pool_register(Cpu *c) { /* RegisterSampler (pool, int id, const NVNsampler *) */
    Sampler *s = PTR(c->x[2]);
    if (s->magic != SAMP_MAGIC) hos_stop(c, "nvn: %#llx is not an initialized NVNsampler", (unsigned long long)c->x[2]);
    memcpy(desc_entry(c, c->x[0], 1, (int)c->x[1]), s, sizeof *s);
}

static void sampler_pool_register_builder(Cpu *c) { /* RegisterSamplerBuilder (pool, int id, builder) */
    Sampler s = {SAMP_MAGIC, 0, {0}};
    memcpy(s.builder, PTR(c->x[2]), SB_SIZE);
    memcpy(desc_entry(c, c->x[0], 1, (int)c->x[1]), &s, sizeof s);
}

static void desc_pool_finalize(Cpu *c) { (void)c; } /* the table is replaced when its memory is initialized again */

/* ---- programs ------------------------------------------------------------------------------------------------- */
/* Shaders are kept as the game hands them (NVNshaderData: GPU address of the code, control section pointer) for the
 * translator. Out of line: only {magic, host pointer} are stored in the NVNprogram (at least 0x30 bytes, from the
 * original getters). */
#define PROG_MAGIC MAGIC('P', 'R', 'O', 'G')
typedef struct { uint64_t data, control; } ShaderData;
typedef struct { int count; ShaderData stages[6]; } ProgramState;
typedef struct { uint32_t magic, pad; ProgramState *state; } Program;
FITS(Program, 0x30);

static Program *program_of(Cpu *c, uint64_t addr) {
    Program *p = PTR(addr);
    if (!addr || p->magic != PROG_MAGIC) hos_stop(c, "nvn: %#llx is not an initialized NVNprogram", (unsigned long long)addr);
    return p;
}

static void program_initialize(Cpu *c) { /* NVNboolean (NVNprogram *, NVNdevice *) */
    ProgramState *s = calloc(1, sizeof *s);
    if (!s) hos_stop(c, "nvn: out of host memory");
    *(Program *)PTR(c->x[0]) = (Program){PROG_MAGIC, 0, s};
    c->x[0] = 1;
}

static void program_finalize(Cpu *c) {
    Program *p = program_of(c, c->x[0]);
    free(p->state);
    p->magic = 0;
}

static void program_set_shaders(Cpu *c) { /* NVNboolean (NVNprogram *, int n, const NVNshaderData *) */
    Program *p = program_of(c, c->x[0]);
    int n = (int)c->x[1];
    if (n < 0 || n > 6) hos_stop(c, "nvn: ProgramSetShaders with %d stages", n);
    p->state->count = n;
    memcpy(p->state->stages, PTR(c->x[2]), sizeof(ShaderData) * (size_t)n);
    c->x[0] = 1;
}

/* ---- command buffers ------------------------------------------------------------------------------------------ */
/* Recording goes to a host buffer, never into the game's command memory: the game budgets that memory for Maxwell
 * encodings, and several command buffers may share it. Each command is a Record: the proc's host-function index and
 * its argument registers (x0..x7, low halves of v0..v3), taken when the command is recorded. Pointer arguments are
 * kept as addresses only; the Vulkan replay, when it exists, has to capture what it needs per command at record
 * time. EndRecording copies the recording into a Handle, the NVNcommandHandle the game submits. A handle lives
 * until its command buffer adds the control memory it was recorded into again, or is finalized. */
#define CMD_MAGIC MAGIC('C', 'M', 'D', 'B')
#define HANDLE_MAGIC MAGIC('H', 'N', 'D', 'L')
typedef struct { uint32_t proc, pad; uint64_t x[8], v[4]; } Record;
typedef struct Handle {
    uint32_t magic, count;
    struct Handle *next;   /* the command buffer's live handles */
    uint64_t control;      /* control memory chunk current at EndRecording */
    Record records[];
} Handle;
typedef struct {
    Record *rec;
    size_t count, cap;
    int recording;
    uint64_t command_pool, command_offset, command_size, control, control_size;
    uint64_t callback, callback_data;
    int32_t copy_row_stride, copy_image_stride;
    Handle *handles;
} CmdState;
typedef struct { uint32_t magic, pad; CmdState *state; } CmdBuf; /* NVNcommandBuffer: at least 0x90 bytes */
FITS(CmdBuf, 0x90);

static CmdState *cmd_of(Cpu *c, uint64_t addr) {
    CmdBuf *b = PTR(addr);
    if (!addr || b->magic != CMD_MAGIC) hos_stop(c, "nvn: %#llx is not an initialized NVNcommandBuffer", (unsigned long long)addr);
    return b->state;
}

static void free_handles(CmdState *s, uint64_t lo, uint64_t hi, int all) {
    for (Handle **h = &s->handles; *h;) {
        if (all || ((*h)->control >= lo && (*h)->control < hi)) {
            Handle *dead = *h;
            *h = dead->next;
            dead->magic = 0;
            free(dead);
        } else {
            h = &(*h)->next;
        }
    }
}

static void cmd_initialize(Cpu *c) { /* NVNboolean (NVNcommandBuffer *, NVNdevice *) */
    CmdState *s = calloc(1, sizeof *s);
    if (!s) hos_stop(c, "nvn: out of host memory");
    *(CmdBuf *)PTR(c->x[0]) = (CmdBuf){CMD_MAGIC, 0, s};
    c->x[0] = 1;
}

static void cmd_finalize(Cpu *c) {
    CmdBuf *b = PTR(c->x[0]);
    if (b->magic != CMD_MAGIC) return;
    free_handles(b->state, 0, 0, 1);
    free(b->state->rec);
    free(b->state);
    b->magic = 0;
}

static void cmd_add_command_memory(Cpu *c) { /* (cmd, const NVNmemoryPool *, ptrdiff_t offset, size_t size) */
    CmdState *s = cmd_of(c, c->x[0]);
    Pool *p = pool_of(c, c->x[1]);
    if (c->x[2] > p->size || c->x[3] > p->size - c->x[2]) hos_stop(c, "nvn: command memory outside its pool");
    s->command_pool = c->x[1]; s->command_offset = c->x[2]; s->command_size = c->x[3];
}

static void cmd_add_control_memory(Cpu *c) { /* (cmd, void *memory, size_t size) */
    CmdState *s = cmd_of(c, c->x[0]);
    free_handles(s, c->x[1], c->x[1] + c->x[2], 0);
    s->control = c->x[1]; s->control_size = c->x[2];
}

/* Command memory is never consumed (see above): all of it stays free, and the out-of-memory callback never runs. */
static void cmd_command_size(Cpu *c) { c->x[0] = cmd_of(c, c->x[0])->command_size; }
static void cmd_control_size(Cpu *c) { c->x[0] = cmd_of(c, c->x[0])->control_size; }
static void cmd_memory_used(Cpu *c) { cmd_of(c, c->x[0]); c->x[0] = 0; }
static void cmd_set_callback(Cpu *c) { cmd_of(c, c->x[0])->callback = c->x[1]; }
static void cmd_set_callback_data(Cpu *c) { cmd_of(c, c->x[0])->callback_data = c->x[1]; }
static void cmd_get_callback(Cpu *c) { c->x[0] = cmd_of(c, c->x[0])->callback; }
static void cmd_get_callback_data(Cpu *c) { c->x[0] = cmd_of(c, c->x[0])->callback_data; }
static void cmd_set_copy_row_stride(Cpu *c) { cmd_of(c, c->x[0])->copy_row_stride = (int32_t)c->x[1]; }
static void cmd_set_copy_image_stride(Cpu *c) { cmd_of(c, c->x[0])->copy_image_stride = (int32_t)c->x[1]; }
static void cmd_get_copy_row_stride(Cpu *c) { c->x[0] = (uint64_t)(int64_t)cmd_of(c, c->x[0])->copy_row_stride; }
static void cmd_get_copy_image_stride(Cpu *c) { c->x[0] = (uint64_t)(int64_t)cmd_of(c, c->x[0])->copy_image_stride; }
static void cmd_is_recording(Cpu *c) { c->x[0] = (uint64_t)cmd_of(c, c->x[0])->recording; }

static void cmd_begin(Cpu *c) {
    CmdState *s = cmd_of(c, c->x[0]);
    if (s->recording) hos_stop(c, "nvn: BeginRecording on a command buffer that is recording");
    s->recording = 1;
    s->count = 0;
}

static void cmd_end(Cpu *c) { /* NVNcommandHandle (NVNcommandBuffer *) */
    CmdState *s = cmd_of(c, c->x[0]);
    if (!s->recording) hos_stop(c, "nvn: EndRecording without BeginRecording");
    Handle *h = malloc(sizeof *h + s->count * sizeof(Record));
    if (!h) hos_stop(c, "nvn: out of host memory for a recording of %zu commands", s->count);
    h->magic = HANDLE_MAGIC;
    h->count = (uint32_t)s->count;
    h->control = s->control;
    memcpy(h->records, s->rec, s->count * sizeof(Record));
    h->next = s->handles;
    s->handles = h;
    s->recording = 0;
    c->x[0] = (uint64_t)(uintptr_t)h;
}

static void cmd_record(Cpu *c) { /* every command without its own handler */
    CmdState *s = cmd_of(c, c->x[0]);
    if (!s->recording) hos_stop(c, "nvn: %s outside BeginRecording/EndRecording", rt_hostfn_name(c->pc));
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 256;
        Record *r = realloc(s->rec, cap * sizeof *r);
        if (!r) hos_stop(c, "nvn: out of host memory for command recording");
        s->rec = r; s->cap = cap;
    }
    Record *r = &s->rec[s->count++];
    r->proc = (uint32_t)((c->pc - DKR_HOSTFN_BASE) / 4);
    r->pad = 0;
    memcpy(r->x, c->x, sizeof r->x);
    for (int i = 0; i < 4; i++) memcpy(&r->v[i], &c->v[i], 8);
}

static void queue_submit(Cpu *c) { /* void (NVNqueue *, int n, const NVNcommandHandle *) */
    queue_of(c, c->x[0]);
    int n = (int)c->x[1];
    if (n < 0 || (n && !c->x[2])) hos_stop(c, "nvn: QueueSubmitCommands with %d handles at %#llx", n, (unsigned long long)c->x[2]);
    for (int i = 0; i < n; i++) {
        Handle *h = PTR(rd64(c->x[2] + 8u * (unsigned)i));
        if (!h || h->magic != HANDLE_MAGIC) hos_stop(c, "nvn: submitted command handle %d is not live", i);
    }
    InterlockedIncrement64(&g_submits); /* no back end yet: the recording is accepted, not replayed */
}

/* ---- windows and presentation --------------------------------------------------------------------------------- */
/* NVNwindowBuilder (sdk+0x2ea4e0..0x2ea50c): device +0, texture count +8, texture pointer array +0x10, native window
 * +0x18, present interval - 1 at +0x20. The NVNwindow (at least 0xc0 bytes from the original getters) holds
 * {magic, host pointer}. Present paces to 60 Hz / interval, as the display would; with no renderer yet it shows
 * nothing and only counts frames. */
#define WIN_MAGIC MAGIC('W', 'I', 'N', 'D')
#define WIN_MAX_TEXTURES 8
typedef struct {
    int count, next, interval;
    int32_t crop[4];
    uint64_t textures[WIN_MAX_TEXTURES], native;
    LARGE_INTEGER deadline;
    uint64_t frames;
} WindowState;
typedef struct { uint32_t magic, pad; WindowState *state; } Window;
FITS(Window, 0xc0);

static WindowState *window_of(Cpu *c, uint64_t addr) {
    Window *w = PTR(addr);
    if (!addr || w->magic != WIN_MAGIC) hos_stop(c, "nvn: %#llx is not an initialized NVNwindow", (unsigned long long)addr);
    return w->state;
}

static void window_initialize(Cpu *c) { /* NVNboolean (NVNwindow *, const NVNwindowBuilder *) */
    uint64_t b = c->x[1];
    int n = (int)rd32(b + 8);
    if (n < 1 || n > WIN_MAX_TEXTURES) hos_stop(c, "nvn: window with %d textures", n);
    WindowState *s = calloc(1, sizeof *s);
    if (!s) hos_stop(c, "nvn: out of host memory");
    s->count = n;
    s->interval = (int)rd32(b + 0x20) + 1;
    s->native = rd64(b + 0x18);
    for (int i = 0; i < n; i++) {
        s->textures[i] = rd64(rd64(b + 0x10) + 8u * (unsigned)i);
        Texture *t = texture_of(c, s->textures[i]);
        if (!i) { s->crop[2] = (int32_t)tex32(t, TB_WIDTH); s->crop[3] = (int32_t)tex32(t, TB_HEIGHT); }
    }
    *(Window *)PTR(c->x[0]) = (Window){WIN_MAGIC, 0, s};
    hos_log("nvn: Window %#llx: %d textures %dx%d, present interval %d, native window %#llx\n",
            (unsigned long long)c->x[0], n, s->crop[2], s->crop[3], s->interval, (unsigned long long)s->native);
    c->x[0] = 1;
}

static void window_finalize(Cpu *c) {
    Window *w = PTR(c->x[0]);
    if (w->magic != WIN_MAGIC) return;
    free(w->state);
    w->magic = 0;
}

static int acquire_index(WindowState *s) { int i = s->next; s->next = (s->next + 1) % s->count; return i; }

/* NVNwindowAcquireTextureResult (NVNwindow *, NVNsync *, int *index): the texture is free at once, the sync too */
static void window_acquire(Cpu *c) {
    WindowState *s = window_of(c, c->x[0]);
    Sync *sync = sync_of(c, c->x[1]);
    sync->fenced = 1;
    sync->fence = (uint64_t)g_submits;
    int32_t i = acquire_index(s);
    memcpy(PTR(c->x[2]), &i, 4);
    c->x[0] = 0; /* SUCCESS */
}

/* NVNqueueAcquireTextureResult (NVNqueue *, NVNwindow *, int *index): the legacy queue-side acquire */
static void queue_acquire(Cpu *c) {
    queue_of(c, c->x[0]);
    int32_t i = acquire_index(window_of(c, c->x[1]));
    memcpy(PTR(c->x[2]), &i, 4);
    c->x[0] = 0;
}

static void queue_present(Cpu *c) { /* void (NVNqueue *, NVNwindow *, int index) */
    queue_of(c, c->x[0]);
    WindowState *s = window_of(c, c->x[1]);
    int i = (int)c->x[2];
    if (i < 0 || i >= s->count) hos_stop(c, "nvn: present of texture %d of %d", i, s->count);
    LARGE_INTEGER now, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    LONGLONG period = freq.QuadPart * (s->interval > 0 ? s->interval : 1) / 60;
    if (!s->deadline.QuadPart || now.QuadPart - s->deadline.QuadPart > period * 4) s->deadline = now;
    s->deadline.QuadPart += period;
    LONGLONG wait = s->deadline.QuadPart - now.QuadPart;
    if (wait > 0) Sleep((DWORD)(wait * 1000 / freq.QuadPart));
    if (++s->frames == 1 || s->frames % 600 == 0)
        hos_log("nvn: present #%llu (texture %d); no renderer yet, nothing is shown\n", (unsigned long long)s->frames, i);
}

static void window_get_interval(Cpu *c) { c->x[0] = (uint64_t)window_of(c, c->x[0])->interval; }
static void window_set_interval(Cpu *c) { window_of(c, c->x[0])->interval = (int)c->x[1]; }
static void window_get_native(Cpu *c) { c->x[0] = window_of(c, c->x[0])->native; }

static void window_set_crop(Cpu *c) { /* void (NVNwindow *, int x, int y, int w, int h) */
    WindowState *s = window_of(c, c->x[0]);
    for (int i = 0; i < 4; i++) s->crop[i] = (int32_t)c->x[1 + i];
}

static void window_get_crop(Cpu *c) { /* void (const NVNwindow *, int *x, int *y, int *w, int *h) */
    WindowState *s = window_of(c, c->x[0]);
    for (int i = 0; i < 4; i++) memcpy(PTR(c->x[1 + i]), &s->crop[i], 4);
}

const NvnImpl dkr_nvn_obj_impls[] = {
    {"nvnWindowInitialize", window_initialize},
    {"nvnWindowFinalize", window_finalize},
    {"nvnWindowAcquireTexture", window_acquire},
    {"nvnQueueAcquireTexture", queue_acquire},
    {"nvnQueuePresentTexture", queue_present},
    {"nvnWindowGetPresentInterval", window_get_interval},
    {"nvnWindowSetPresentInterval", window_set_interval},
    {"nvnWindowGetNativeWindow", window_get_native},
    {"nvnWindowSetCrop", window_set_crop},
    {"nvnWindowGetCrop", window_get_crop},
    {"nvnCommandBufferInitialize", cmd_initialize},
    {"nvnCommandBufferFinalize", cmd_finalize},
    {"nvnCommandBufferAddCommandMemory", cmd_add_command_memory},
    {"nvnCommandBufferAddControlMemory", cmd_add_control_memory},
    {"nvnCommandBufferGetCommandMemorySize", cmd_command_size},
    {"nvnCommandBufferGetCommandMemoryUsed", cmd_memory_used},
    {"nvnCommandBufferGetCommandMemoryFree", cmd_command_size},
    {"nvnCommandBufferGetControlMemorySize", cmd_control_size},
    {"nvnCommandBufferGetControlMemoryUsed", cmd_memory_used},
    {"nvnCommandBufferGetControlMemoryFree", cmd_control_size},
    {"nvnCommandBufferSetMemoryCallback", cmd_set_callback},
    {"nvnCommandBufferSetMemoryCallbackData", cmd_set_callback_data},
    {"nvnCommandBufferGetMemoryCallback", cmd_get_callback},
    {"nvnCommandBufferGetMemoryCallbackData", cmd_get_callback_data},
    {"nvnCommandBufferSetCopyRowStride", cmd_set_copy_row_stride},
    {"nvnCommandBufferSetCopyImageStride", cmd_set_copy_image_stride},
    {"nvnCommandBufferGetCopyRowStride", cmd_get_copy_row_stride},
    {"nvnCommandBufferGetCopyImageStride", cmd_get_copy_image_stride},
    {"nvnCommandBufferIsRecording", cmd_is_recording},
    {"nvnCommandBufferBeginRecording", cmd_begin},
    {"nvnCommandBufferEndRecording", cmd_end},
    {"nvnQueueSubmitCommands", queue_submit},
    {"nvnTextureInitialize", texture_initialize},
    {"nvnTextureFinalize", texture_finalize},
    {"nvnTextureGetWidth", texture_get_width},
    {"nvnTextureGetHeight", texture_get_height},
    {"nvnTextureGetDepth", texture_get_depth},
    {"nvnTextureGetLevels", texture_get_levels},
    {"nvnTextureGetFormat", texture_get_format},
    {"nvnTextureGetTarget", texture_get_target},
    {"nvnTextureGetFlags", texture_get_flags},
    {"nvnTextureGetSamples", texture_get_samples},
    {"nvnTextureGetStride", texture_get_stride},
    {"nvnTextureGetMemoryPool", texture_get_pool},
    {"nvnTextureGetMemoryOffset", texture_get_offset},
    {"nvnTextureGetStorageSize", texture_get_storage_size},
    {"nvnTextureGetDepthStencilMode", texture_get_ds_mode},
    {"nvnTextureGetSwizzle", texture_get_swizzle},
    {"nvnTextureGetStorageClass", texture_get_storage_class},
    {"nvnTextureBuilderGetStorageSize", builder_storage_size},
    {"nvnTextureBuilderGetStorageAlignment", builder_storage_alignment},
    {"nvnTextureBuilderGetStorageClass", builder_storage_class},
    {"nvnSamplerInitialize", sampler_initialize},
    {"nvnSamplerFinalize", sampler_finalize},
    {"nvnTexturePoolInitialize", texture_pool_initialize},
    {"nvnTexturePoolFinalize", desc_pool_finalize},
    {"nvnTexturePoolRegisterTexture", texture_pool_register},
    {"nvnTexturePoolRegisterImage", texture_pool_register},
    {"nvnSamplerPoolInitialize", sampler_pool_initialize},
    {"nvnSamplerPoolFinalize", desc_pool_finalize},
    {"nvnSamplerPoolRegisterSampler", sampler_pool_register},
    {"nvnSamplerPoolRegisterSamplerBuilder", sampler_pool_register_builder},
    {"nvnProgramInitialize", program_initialize},
    {"nvnProgramFinalize", program_finalize},
    {"nvnProgramSetShaders", program_set_shaders},
    {"nvnSyncInitialize", sync_initialize},
    {"nvnSyncFinalize", sync_finalize},
    {"nvnSyncWait", sync_wait},
    {"nvnQueueFenceSync", queue_fence_sync},
    {"nvnQueueWaitSync", queue_wait_sync},
    {"nvnMemoryPoolInitialize", pool_initialize},
    {"nvnMemoryPoolFinalize", pool_finalize},
    {"nvnMemoryPoolMap", pool_map},
    {"nvnMemoryPoolGetBufferAddress", pool_buffer_address},
    {"nvnMemoryPoolGetFlags", pool_get_flags},
    {"nvnMemoryPoolGetSize", pool_get_size},
    {"nvnMemoryPoolFlushMappedRange", pool_mapped_range},
    {"nvnMemoryPoolInvalidateMappedRange", pool_mapped_range},
    {"nvnBufferInitialize", buffer_initialize},
    {"nvnBufferFinalize", buffer_finalize},
    {"nvnBufferMap", buffer_map},
    {"nvnBufferGetAddress", buffer_get_address},
    {"nvnBufferGetMemoryPool", buffer_get_pool},
    {"nvnBufferGetMemoryOffset", buffer_get_offset},
    {"nvnBufferGetSize", buffer_get_size},
    {"nvnBufferFlushMappedRange", buffer_mapped_range},
    {"nvnBufferInvalidateMappedRange", buffer_mapped_range},
    {"nvnEventInitialize", event_initialize},
    {"nvnEventFinalize", event_finalize},
    {"nvnEventGetValue", event_get_value},
    {"nvnEventSignal", event_signal},
    {"nvnQueueInitialize", queue_initialize},
    {"nvnQueueFinalize", queue_finalize},
    {"nvnQueueFlush", queue_flush},
    {"nvnQueueFinish", queue_finish},
    {"nvnQueueGetError", queue_get_error},
};
const size_t dkr_nvn_obj_impl_count = sizeof dkr_nvn_obj_impls / sizeof *dkr_nvn_obj_impls;

/* Commands without their own handler: every other nvnCommandBuffer* proc. */
GuestFn dkr_nvn_obj_fallback(const char *name) {
    return !strncmp(name, "nvnCommandBuffer", 16) ? cmd_record : NULL;
}
