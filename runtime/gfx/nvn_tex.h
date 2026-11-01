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
typedef struct NvnTexLevel {
    uint32_t width, height, depth, block_width, block_height, row_bytes, rows, linear_stride;
    uint64_t offset, linear_size;
    unsigned tile_h, tile_d;
} NvnTexLevel;
int nvn_tex_level(const NvnTexDesc *desc, unsigned level, unsigned layer, NvnTexLevel *out);
uint64_t nvn_tex_block_offset(uint32_t x_byte, uint32_t row, uint32_t slice, uint32_t row_bytes, uint32_t rows, unsigned tile_h, unsigned tile_d);
int nvn_tex_linearize(const NvnTexDesc *desc, const void *storage, uint64_t storage_size, unsigned level, unsigned layer,
                      void *linear, uint64_t linear_size);

#endif
