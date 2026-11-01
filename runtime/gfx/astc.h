#ifndef DKR_ASTC_H
#define DKR_ASTC_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
bool astc_decode_block(const uint8_t block[16], uint32_t bw, uint32_t bh, bool srgb, uint8_t *rgba);
void astc_decode_ldr(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t bw, uint32_t bh, bool srgb, uint8_t *rgba);
#endif
