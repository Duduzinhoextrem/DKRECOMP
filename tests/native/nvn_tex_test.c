/* Block-linear texture storage sizes of runtime/gfx/nvn_tex.c against original-SDK queries.
 * The 40 size and block-height cases are from TotK Recomp runtime/nvn_tex_probe.c
 * @ c3b9a4e7f6ec3d1f0a8c84b66b1703b15639a784 (MIT), ported from MacNativeGames
 * games/totk/tests/texture_auto_layout/probe.c @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou). They were measured
 * with a newer NVN SDK (55.x); the Tegra layout is hardware-defined, but the automatic block-height choice of NVN
 * 53.105 is not verified by them. No game bytes. */
#include "../../runtime/gfx/nvn_tex.h"
#include "../../runtime/gfx/astc.h"
#include <stdio.h>
#include <string.h>

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

static int test_reading(void) {
    int failures = 0;
#define T(condition) do { if (!(condition)) { printf("FAIL texture read line %d: %s\n", __LINE__, #condition); failures++; } } while (0)
    T(nvn_tex_block_offset(16, 0, 0, 256, 32, 1, 1) == 32);
    T(nvn_tex_block_offset(32, 0, 0, 256, 32, 1, 1) == 256);
    T(nvn_tex_block_offset(0, 1, 0, 256, 32, 1, 1) == 16);
    T(nvn_tex_block_offset(64, 0, 0, 256, 32, 1, 1) == 2048);
    T(nvn_tex_block_offset(0, 16, 0, 256, 32, 1, 1) == 8192);
    T(nvn_tex_block_offset(0, 0, 1, 256, 32, 1, 1) == 1024);
    NvnTexDesc desc = {.target = 1, .format = 0x25, .width = 19, .height = 17, .depth = 1, .levels = 3};
    NvnTexLevel level;
    T(!nvn_tex_level(&desc, 1, 0, &level));
    T(level.width == 9 && level.height == 8 && level.offset == 4096 && level.tile_h == 0 && level.linear_size == 288);
    T(!nvn_tex_level(&desc, 2, 0, &level) && level.offset == 4608);
    T(nvn_tex_level(&desc, 3, 0, &level) == -1);
    T(nvn_tex_level(&desc, 0, 1, &level) == -1);
    uint8_t blocks[4][16], storage[512] = {0}, linear[64], output[7*6*4 + 32];
    const uint8_t colors[4][4] = {{255,0,0,255}, {0,255,0,255}, {0,0,255,255}, {255,255,255,255}};
    const unsigned offsets[] = {0, 32, 16, 48};
    for (unsigned i = 0; i < 4; i++) {
        memset(blocks[i], 0xff, 8); blocks[i][0] = 0xfc; blocks[i][1] = 0xfd; /* UNORM16 void extent */
        for (unsigned c = 0; c < 4; c++) blocks[i][8+2*c] = blocks[i][9+2*c] = colors[i][c];
        memcpy(storage + offsets[i], blocks[i], 16);
    }
    desc = (NvnTexDesc){.target = 1, .format = 0x7b, .width = 7, .height = 6, .depth = 1, .levels = 1};
    T(!nvn_tex_linearize(&desc, storage, sizeof storage, 0, 0, linear, sizeof linear));
    T(!memcmp(linear, blocks, sizeof blocks));
    T(nvn_tex_linearize(&desc, storage, 45, 0, 0, linear, sizeof linear) == -1);
    T(nvn_tex_linearize(&desc, storage, sizeof storage, 0, 0, linear, 63) == -1);
    memset(output, 0xa5, sizeof output);
    astc_decode_ldr((const uint8_t *)blocks, 7, 6, 5, 5, false, output + 16);
    for (unsigned y = 0; y < 6; y++) for (unsigned x = 0; x < 7; x++)
        T(!memcmp(output + 16 + 4*(y*7+x), colors[(y>=5)*2 + (x>=5)], 4));
    for (unsigned i = 0; i < 16; i++) T(output[i] == 0xa5 && output[sizeof output - 16 + i] == 0xa5);
    uint8_t one[12*12*4], bad[16] = {0};
    for (unsigned footprint = 4; footprint <= 12; footprint++) {
        T(astc_decode_block(blocks[0], footprint, footprint, true, one));
        for (unsigned i = 0; i < footprint*footprint; i++) T(!memcmp(one + i*4, colors[0], 4));
    }
    T(!astc_decode_block(bad, 5, 5, false, one));
    const uint8_t magenta[] = {255,0,255,255};
    for (unsigned i = 0; i < 25; i++) T(!memcmp(one + i*4, magenta, 4));
    memcpy(bad, blocks[0], 16); bad[1] |= 2; /* HDR void extent is unavailable in LDR. */
    T(!astc_decode_block(bad, 5, 5, false, one));
    for (unsigned i = 0; i < 25; i++) T(!memcmp(one + i*4, magenta, 4));
#undef T
    return failures;
}

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
    failures += test_reading();
    printf("NVN texture layout: %s (%u SDK size cases, packaged/pitch, deswizzle and ASTC)\n", failures ? "FAIL" : "PASS",
           (unsigned)(sizeof cases / sizeof *cases));
    return failures ? 1 : 0;
}
