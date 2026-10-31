/* NVN texture storage layout (Tegra X1 block-linear); see nvn_tex.c for origin and licence. */
#ifndef DKR_NVN_TEX_H
#define DKR_NVN_TEX_H

#include <stdint.h>

typedef struct {
    uint32_t flags, target, width, height, depth, levels, format, samples, stride;
    int packaged;                       /* SetPackagedTextureLayout was called */
    unsigned packaged_tile_h, packaged_tile_d;
} NvnTexDesc;

typedef struct {
    uint64_t size, layer_size;          /* total storage bytes, bytes per layer */
    uint32_t align, layers, bpb;        /* alignment, layer count, bytes per block (0: unknown format) */
    unsigned tile_h, tile_d;            /* block height / depth in GOBs, log2 */
} NvnTexLayout;

void nvn_tex_layout(const NvnTexDesc *d, NvnTexLayout *out);
int nvn_tex_bytes_per_block(int format); /* 0 for an unknown format */

#endif
