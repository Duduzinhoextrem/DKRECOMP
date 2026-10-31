/* Storage size, alignment and level offsets of NVN textures in the Tegra X1 block-linear layout.
 *
 * Adapted from TotK Recomp runtime/nvn_tex.c @ c3b9a4e7f6ec3d1f0a8c84b66b1703b15639a784 (MIT, Copyright (c) 2026
 * Valiantsin Dzerakh), itself ported from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/nvn_tex.c
 * @ b75be8cfa704df3701e86130c8ced268e79d6b11, based on deko3d source/dk_image.cpp. Changes here: descriptors come
 * from this game's NVNtextureBuilder layout (nvn_obj.c), the configuration hook and the TotK-only switch for
 * MINIMAL_LAYOUT were dropped (the rule is kept, unverified for NVN 53.105). Original notice:
 *
 * Copyright (C) 2018-2020 fincs
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any
 * damages arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any
 * purpose, including commercial applications, and to alter it and
 * redistribute it freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you
 *    must not claim that you wrote the original software. If you use
 *    this software in a product, an acknowledgment in the product
 *    documentation would be appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and
 *    must not be misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source
 *    distribution.
 *
 * One GOB is 64 bytes x 8 rows (512 bytes). The block height (in GOBs, log2) is chosen from height x 1.5 truncated
 * to whole GOB rows, as the NVN SDK does; packaged textures carry their own block height and depth. */
#include "nvn_tex.h"

typedef struct { uint8_t bpb, bw, bh; } Fmt; /* bytes per block, block width and height (1x1 when uncompressed) */

static Fmt fmt_of(int f) {
    static const uint8_t astc[14][2] = {{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
    if (f >= 0x79 && f <= 0x94) { int k = (f - 0x79) % 14; return (Fmt){16, astc[k][0], astc[k][1]}; }
    if (f >= 0x41 && f <= 0x50) { /* DXT / RGTC / BPTC: 4x4 blocks */
        int b8 = (f <= 0x42) || (f == 0x45 || f == 0x46) || (f == 0x49 || f == 0x4A);
        return (Fmt){(uint8_t)(b8 ? 8 : 16), 4, 4};
    }
    static const struct { uint8_t lo, hi, bpb; } r[] = {
        {0x01,0x04,1},{0x05,0x09,2},{0x0A,0x0C,4},{0x0D,0x10,2},{0x11,0x15,4},{0x16,0x18,8},
        {0x19,0x1C,3},{0x1D,0x21,6},{0x22,0x24,12},{0x25,0x28,4},{0x29,0x2D,8},{0x2E,0x30,16},
        {0x31,0x31,1},{0x32,0x32,2},{0x33,0x35,4},{0x36,0x36,8},{0x37,0x38,4},{0x39,0x3C,2},{0x3D,0x40,4},
        {0x51,0x52,1},{0x53,0x54,2},{0x55,0x56,4},{0x57,0x58,2},{0x59,0x5A,4},{0x5B,0x5C,8},
        {0x5D,0x5E,3},{0x5F,0x60,6},{0x61,0x62,12},{0x63,0x64,4},{0x65,0x66,8},{0x67,0x68,16},
        {0x69,0x6C,4},{0x6D,0x70,4},{0x71,0x75,8},{0x76,0x78,16},{0x95,0x98,2},{0x99,0x9C,4},
    };
    for (unsigned i = 0; i < sizeof r / sizeof *r; i++)
        if (f >= r[i].lo && f <= r[i].hi) return (Fmt){r[i].bpb, 1, 1};
    return (Fmt){0, 1, 1};
}

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }
static uint32_t blk(uint32_t s, uint32_t b) { return (s + b - 1) / b; }
static unsigned pick_tile(uint32_t gobs) { return gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0; }

static unsigned adj_tile(unsigned shift, uint32_t unit, uint32_t dim) {
    if (!shift) return 0;
    uint32_t x = unit << (shift - 1);
    if (x >= dim) while (--shift) { x >>= 1; if (x < dim) break; }
    return shift;
}

/* NVNtextureTarget: dimensions per level and whether layers are stored one after another */
static void shape(const NvnTexDesc *d, unsigned *dpl, int *layered, uint32_t *w, uint32_t *h, uint32_t *z) {
    *w = d->width; *h = d->height; *z = d->depth; *layered = 0;
    switch (d->target) {
    case 0: *dpl = 1; *h = 1; *z = 1; break;                       /* 1D */
    case 3: *dpl = 1; *z = d->height; *h = 1; *layered = 1; break; /* 1D_ARRAY: layers in height */
    case 2: *dpl = 3; break;                                        /* 3D */
    case 4: case 6: *dpl = 2; *layered = 1; break;                  /* 2D_ARRAY, 2D_MULTISAMPLE_ARRAY */
    case 8: *dpl = 2; *z = 6; *layered = 1; break;                  /* CUBEMAP */
    case 9: *dpl = 2; *layered = 1; break;                          /* CUBEMAP_ARRAY: depth counts faces */
    default: *dpl = 2; *z = 1; break;                               /* 2D, 2D_MULTISAMPLE, RECTANGLE */
    }
    if (*z == 0) *z = 1;
}

static void samples_xy(int s, uint32_t *sx, uint32_t *sy) {
    *sx = *sy = 1;
    if (s == 2) *sx = 2;
    else if (s == 4) *sx = *sy = 2;
    else if (s == 8) { *sx = 4; *sy = 2; }
}

static uint64_t level_offset(const NvnTexDesc *d, unsigned level, unsigned dpl, uint32_t w, uint32_t h, uint32_t z,
                             unsigned tile_h, unsigned tile_d, Fmt t) {
    uint32_t sx, sy;
    samples_xy(d->samples, &sx, &sy);
    uint64_t off = 0;
    for (unsigned i = 0; i < level; i++) {
        uint32_t lw = blk(mip(w * sx, i), t.bw);
        uint32_t lh = dpl >= 2 ? blk(mip(h * sy, i), t.bh) : 1;
        uint32_t ld = dpl >= 3 ? mip(z, i) : 1;
        unsigned th = adj_tile(tile_h, 8, lh), td = adj_tile(tile_d, 1, ld);
        uint32_t wg = (lw * t.bpb + 63) / 64, hg = (lh + 7) / 8;
        uint32_t ht = (hg + (1u << th) - 1) >> th, dt = (ld + (1u << td) - 1) >> td;
        off += (uint64_t)wg * ht * dt << (9 + th + td);
    }
    return off;
}

static uint64_t layer_bytes(const NvnTexDesc *d, unsigned dpl, int layered, uint32_t w, uint32_t h, uint32_t z,
                            unsigned tile_h, unsigned tile_d, Fmt t) {
    uint64_t ls = level_offset(d, d->levels ? d->levels : 1, dpl, w, h, z, tile_h, tile_d, t);
    if (layered) {
        uint32_t hh = blk(h, t.bh);
        unsigned th = tile_h, td = tile_d;
        while (th && hh <= (8u << (th - 1))) th--;
        while (td && z <= (1u << (td - 1))) td--;
        uint64_t align = 1ull << (th + td + 9);
        ls = (ls + align - 1) & ~(align - 1);
    }
    return ls;
}

int nvn_tex_bytes_per_block(int format) { return fmt_of(format).bpb; }

void nvn_tex_layout(const NvnTexDesc *d, NvnTexLayout *o) {
    Fmt t = fmt_of(d->format);
    unsigned dpl;
    int layered;
    uint32_t w, h, z, sx, sy;
    shape(d, &dpl, &layered, &w, &h, &z);
    samples_xy(d->samples, &sx, &sy);
    o->bpb = t.bpb; o->tile_h = o->tile_d = 0; o->layers = layered ? z : 1;
    if (!t.bpb) { o->size = 0; o->align = 512; o->layer_size = 0; return; }
    if (d->target == 10) { /* BUFFER */
        o->layer_size = o->size = (uint64_t)t.bpb * d->width;
        o->align = t.bpb == 12 ? 4 : t.bpb;
        return;
    }
    if (d->flags & 0x50) { /* LINEAR or LINEAR_RENDER_TARGET: pitch storage */
        o->align = (d->flags & 0x40) ? 128 : 32;
        uint32_t stride = d->stride ? d->stride : (blk(d->width, t.bw) * t.bpb + o->align - 1) & ~(o->align - 1);
        o->layer_size = o->size = (uint64_t)stride * h; /* the driver counts pixel rows, also when compressed */
        return;
    }
    unsigned tile_h = 0, tile_d = 0;
    if (d->packaged) { /* SetPackagedTextureLayout: block height and depth chosen offline */
        if (dpl >= 2) tile_h = d->packaged_tile_h;
        if (dpl == 3) tile_d = d->packaged_tile_d;
    } else if (dpl == 2) {
        uint32_t ah = blk(h * sy, t.bh);
        tile_h = pick_tile((ah + ah / 2) / 8);
    } else if (dpl == 3) {
        tile_d = pick_tile(z + z / 2);
    }
    uint64_t ls = layer_bytes(d, dpl, layered, w, h, z, tile_h, tile_d, t);
    if ((d->flags & 0x200) && !d->packaged) { /* MINIMAL_LAYOUT (rule measured on NVN 55, unverified on 53.105) */
        uint64_t minimal = layer_bytes(d, dpl, layered, w, h, z, 0, 0, t);
        if (minimal != ls) { tile_h = tile_d = 0; ls = minimal; }
    }
    o->tile_h = tile_h; o->tile_d = tile_d;
    o->layer_size = ls;
    o->size = layered ? ls * z : ls;
    o->align = 512;
}
