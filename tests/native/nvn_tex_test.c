/* Block-linear texture storage sizes of runtime/gfx/nvn_tex.c against original-SDK queries.
 * The 40 size and block-height cases are from TotK Recomp runtime/nvn_tex_probe.c
 * @ c3b9a4e7f6ec3d1f0a8c84b66b1703b15639a784 (MIT), ported from MacNativeGames
 * games/totk/tests/texture_auto_layout/probe.c @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou). They were measured
 * with a newer NVN SDK (55.x); the Tegra layout is hardware-defined, but the automatic block-height choice of NVN
 * 53.105 is not verified by them. No game bytes. */
#include "../../runtime/gfx/nvn_tex.h"
#include <stdio.h>

typedef struct { unsigned format, width, height, levels, h; uint64_t size; } Case;
static const Case cases[] = {
    {0x25, 40, 1, 6, 0, 4608}, {0x25, 40, 8, 6, 0, 4608}, {0x25, 40, 9, 6, 0, 6144}, {0x25, 40, 10, 6, 0, 6144},
    {0x25, 40, 11, 6, 1, 6144}, {0x25, 40, 16, 6, 1, 6144}, {0x25, 40, 17, 6, 1, 9216}, {0x25, 40, 21, 6, 1, 10240},
    {0x25, 40, 22, 6, 2, 10240}, {0x25, 40, 32, 6, 2, 10240}, {0x25, 40, 33, 6, 2, 16384}, {0x25, 40, 40, 6, 2, 18944},
    {0x25, 40, 42, 6, 2, 18944}, {0x25, 40, 43, 6, 3, 18944}, {0x25, 40, 64, 7, 3, 19456}, {0x25, 40, 65, 7, 3, 31744},
    {0x25, 40, 85, 7, 3, 37376}, {0x25, 40, 86, 7, 4, 37376}, {0x25, 40, 128, 8, 4, 37888}, {0x25, 40, 129, 8, 4, 62464},
    {0x48, 160, 4, 8, 0, 12288}, {0x48, 160, 32, 8, 0, 12288}, {0x48, 160, 36, 8, 0, 17408}, {0x48, 160, 40, 8, 0, 17408},
    {0x48, 160, 44, 8, 1, 17408}, {0x48, 160, 64, 8, 1, 17408}, {0x48, 160, 68, 8, 1, 30208}, {0x48, 160, 84, 8, 1, 30208},
    {0x48, 160, 88, 8, 2, 30208}, {0x48, 160, 128, 8, 2, 30208}, {0x48, 160, 132, 8, 2, 57344}, {0x48, 160, 160, 8, 2, 57344},
    {0x48, 160, 168, 8, 2, 57344}, {0x48, 160, 172, 8, 3, 57344}, {0x48, 160, 256, 9, 3, 57856}, {0x48, 160, 260, 9, 3, 112128},
    {0x48, 160, 340, 9, 3, 113152}, {0x48, 160, 344, 9, 4, 113152}, {0x48, 160, 512, 10, 4, 113664}, {0x48, 160, 516, 10, 4, 222208},
};

int main(void) {
    int failures = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        const Case *c = &cases[i];
        NvnTexDesc d = {.target = 1, .format = c->format, .samples = 1, .width = c->width, .height = c->height,
                        .depth = 1, .levels = c->levels};
        NvnTexLayout l;
        nvn_tex_layout(&d, &l);
        if (l.tile_h != c->h || l.size != c->size || l.align != 512) {
            printf("FAIL case %u format %#x %ux%u: block height %u size %llu align %u, expected %u %llu 512\n", i,
                   c->format, c->width, c->height, l.tile_h, (unsigned long long)l.size, l.align, c->h,
                   (unsigned long long)c->size);
            failures++;
        }
    }
    /* Packaged layout: the stored block height wins over the automatic choice (160x160 BC3: H2 automatic). */
    NvnTexDesc pad = {.target = 1, .format = 0x48, .samples = 1, .width = 160, .height = 160, .depth = 1, .levels = 8,
                      .packaged = 1, .packaged_tile_h = 3};
    NvnTexLayout l;
    nvn_tex_layout(&pad, &l);
    if (l.tile_h != 3) { printf("FAIL packaged block height %u, expected 3\n", l.tile_h); failures++; }
    /* Pitch storage: LINEAR_RENDER_TARGET (0x40) aligns the stride to 128 bytes, LINEAR (0x10) to 32. */
    NvnTexDesc lin = {.target = 1, .format = 0x25, .flags = 0x40, .samples = 1, .width = 40, .height = 4, .depth = 1, .levels = 1};
    nvn_tex_layout(&lin, &l);
    if (l.size != 1024 || l.align != 128) { printf("FAIL linear render target %llu/%u\n", (unsigned long long)l.size, l.align); failures++; }
    lin.flags = 0x10;
    nvn_tex_layout(&lin, &l);
    if (l.size != 640 || l.align != 32) { printf("FAIL linear %llu/%u\n", (unsigned long long)l.size, l.align); failures++; }
    printf("NVN texture layout: %s (%u SDK size cases, packaged and pitch layouts)\n", failures ? "FAIL" : "PASS",
           (unsigned)(sizeof cases / sizeof *cases));
    return failures ? 1 : 0;
}
