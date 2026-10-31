/* NVN 53.105 argument snapshots. Layouts measured in the target SDK with dkrecomp.nvnmap:
 * states 4/8/24 bytes, vertex attribute/stream 4/8 bytes, TextureView 0x24, CopyRegion 0x18.
 * Function signatures were checked against the public MIT NVN declarations in the TotK reference.
 * NVN objects and GPU buffer addresses keep their API lifetime; transient CPU values are copied here. */
#include "nvn_commands.h"
#include "../src/hos.h"
#include <stdatomic.h>
#include <stdlib.h>

struct NvnPayload { _Atomic unsigned refs; unsigned count; void *blocks[16]; };

static void *copy_data(Cpu *c, NvnRecord *r, uint64_t source, uint64_t bytes, int required) {
    if (!bytes) return NULL;
    if (!source) {
        if (required) hos_stop(c, "nvn: %s requires a CPU argument", r->name);
        return NULL;
    }
    if (bytes > 0x1000000) hos_stop(c, "nvn: %s CPU argument exceeds the 16 MiB capture limit", r->name);
    if (!r->payload) {
        r->payload = calloc(1, sizeof *r->payload);
        if (!r->payload) hos_stop(c, "nvn: out of memory creating command payload");
        atomic_init(&r->payload->refs, 1);
    }
    if (r->payload->count == 16) hos_stop(c, "nvn: too many command payload blocks");
    void *data = malloc((size_t)bytes);
    if (!data) hos_stop(c, "nvn: out of memory capturing %llu bytes", (unsigned long long)bytes);
    memcpy(data, (const void *)(uintptr_t)source, (size_t)bytes);
    r->payload->blocks[r->payload->count++] = data;
    return data;
}

static void arg(Cpu *c, NvnRecord *r, unsigned index, uint64_t bytes, int required) {
    r->x[index] = (uint64_t)(uintptr_t)copy_data(c, r, r->x[index], bytes, required);
}

static uint64_t count(Cpu *c, NvnRecord *r, unsigned index) {
    int32_t n = (int32_t)r->x[index];
    if (n < 0 || n > 0x100000) hos_stop(c, "nvn: %s has invalid array count %d", r->name, n);
    return (uint32_t)n;
}

void nvn_record_retain(NvnRecord *r) {
    if (r->payload) atomic_fetch_add(&r->payload->refs, 1);
}

void nvn_record_release(NvnRecord *r) {
    NvnPayload *p = r->payload;
    if (p && atomic_fetch_sub(&p->refs, 1) == 1) {
        for (unsigned i = 0; i < p->count; i++) free(p->blocks[i]);
        free(p);
    }
    r->payload = NULL;
}

void nvn_record_capture(Cpu *c, NvnRecord *r) {
    memset(r, 0, sizeof *r);
    r->name = rt_hostfn_name(c->pc);
    if (!r->name) hos_stop(c, "nvn: recording an unnamed host proc");
    memcpy(r->x, c->x, sizeof r->x);
    for (unsigned i = 0; i < 4; i++) memcpy(&r->v[i], &c->v[i], 8);
    const char *name = r->name + strlen("nvnCommandBuffer");
#define IS(s) (!strcmp(name, (s)))
    if (IS("BindBlendState") || IS("BindDepthStencilState")) arg(c, r, 1, 8, 1);
    else if (IS("BindColorState") || IS("BindChannelMaskState") || IS("BindPolygonState")) arg(c, r, 1, 4, 1);
    else if (IS("BindMultisampleState")) arg(c, r, 1, 24, 1);
    else if (IS("BindVertexAttribState")) arg(c, r, 2, count(c, r, 1) * 4, 1);
    else if (IS("BindVertexStreamState")) arg(c, r, 2, count(c, r, 1) * 8, 1);
    else if (IS("BindVertexBuffers") || IS("BindTransformFeedbackBuffers")) arg(c, r, 3, count(c, r, 2) * 16, 1);
    else if (IS("BindUniformBuffers") || IS("BindStorageBuffers")) arg(c, r, 4, count(c, r, 3) * 16, 1);
    else if (IS("BindTextures") || IS("BindImages") || IS("BindSeparateTextures") || IS("BindSeparateSamplers"))
        arg(c, r, 4, count(c, r, 3) * 8, 1);
    else if (IS("SetViewports") || IS("SetViewportSwizzles") || IS("SetScissors")) arg(c, r, 3, count(c, r, 2) * 16, 1);
    else if (IS("SetDepthRanges")) arg(c, r, 3, count(c, r, 2) * 8, 1);
    else if (IS("SetInnerTessellationLevels")) arg(c, r, 1, 8, 1);
    else if (IS("SetOuterTessellationLevels") || IS("SetBlendColor")) arg(c, r, 1, 16, 1);
    else if (IS("ClearColor") || IS("ClearColori") || IS("ClearColorui")) arg(c, r, 2, 16, 1);
    else if (IS("CopyBufferToTexture")) { arg(c, r, 3, 0x24, 0); arg(c, r, 4, 0x18, 1); }
    else if (IS("CopyTextureToBuffer")) { arg(c, r, 2, 0x24, 0); arg(c, r, 3, 0x18, 1); }
    else if (IS("CopyTextureToTexture")) {
        arg(c, r, 2, 0x24, 0); arg(c, r, 3, 0x18, 1); arg(c, r, 5, 0x24, 0); arg(c, r, 6, 0x18, 1);
    } else if (IS("ClearTexture") || IS("ClearTexturei") || IS("ClearTextureui")) {
        arg(c, r, 2, 0x24, 0); arg(c, r, 3, 0x18, 1); arg(c, r, 4, 16, 1);
    } else if (IS("DownsampleTextureView") || IS("TiledDownsampleTextureView")) {
        arg(c, r, 2, 0x24, 0); arg(c, r, 4, 0x24, 0);
    } else if (IS("UpdateUniformBuffer")) arg(c, r, 5, r->x[4], 1);
    else if (IS("SetProgramSubroutines")) arg(c, r, 5, count(c, r, 4) * 4, 1);
    else if (IS("SetRenderTargets")) {
        uint64_t n = count(c, r, 1);
        if (n > 8) hos_stop(c, "nvn: more than eight render targets");
        arg(c, r, 2, n * 8, 1); arg(c, r, 3, n * 8, 0);
        uint64_t *views = (uint64_t *)(uintptr_t)r->x[3];
        for (unsigned i = 0; views && i < n; i++)
            views[i] = (uint64_t)(uintptr_t)copy_data(c, r, views[i], 0x24, 0);
        arg(c, r, 5, 0x24, 0);
    } else if (strstr(name, "DebugGroup") || strstr(name, "DebugMarker")) {
        unsigned index = IS("InsertDebugMarker") ? 1 : 2;
        if (strncmp(name, "Pop", 3) && r->x[index]) {
            size_t length = 0;
            const char *text = (const char *)(uintptr_t)r->x[index];
            while (length < 0x10000 && text[length]) length++;
            if (length == 0x10000) hos_stop(c, "nvn: unterminated debug label");
            arg(c, r, index, length + 1, 1);
        }
    } else if (IS("DrawTexture") || IS("BindCoverageModulationTable")) {
        hos_stop(c, "nvn: CPU argument layout of %s needs measurement before capture", r->name);
    }
#undef IS
}
