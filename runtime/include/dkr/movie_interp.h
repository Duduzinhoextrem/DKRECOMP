/* Optional NV12 frame blending. Uses the original timestamps and never blends across a scene cut.
 * This smooths 29.97 fps to 59.94 fps; it is not motion estimation or detail reconstruction. */
#ifndef DKR_MOVIE_INTERP_H
#define DKR_MOVIE_INTERP_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline void dkr_movie_midframe(uint8_t *out, const uint8_t *a, const uint8_t *b,
                                      size_t bytes, size_t luma) {
    uint64_t difference = 0, samples = 0;
    for (size_t i = 0; i < luma; i += 64) {
        difference += a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        samples++;
    }
    if (difference > samples * 32) { memcpy(out, a, bytes); return; }
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        uint64_t va, vb, vm;
        memcpy(&va, a + i, 8);
        memcpy(&vb, b + i, 8);
        vm = (va | vb) - (((va ^ vb) >> 1) & 0x7f7f7f7f7f7f7f7fULL);
        memcpy(out + i, &vm, 8);
    }
    for (; i < bytes; i++) out[i] = (uint8_t)(((unsigned)a[i] + b[i] + 1) / 2);
}

static inline int64_t dkr_movie_midpoint(int64_t previous, int64_t current) {
    /* A seek, duplicate PTS, or an already high-rate stream must not create a new frame. */
    if (current <= previous || previous > INT64_MAX - 50000) return -1;
    if (current < previous + 25000 || current > previous + 50000) return -1;
    return previous + (current - previous) / 2;
}
#endif
