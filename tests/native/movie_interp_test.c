#include "dkr/movie_interp.h"
#include <stdio.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void) {
    uint8_t a[] = {100, 105, 0, 255, 120, 130};
    uint8_t b[] = {110, 110, 1, 254, 130, 140}, out[6];
    dkr_movie_midframe(out, a, b, 6, 4);
    CHECK(out[0] == 105 && out[1] == 108 && out[2] == 1 && out[3] == 255);
    CHECK(out[4] == 125 && out[5] == 135); /* interleaved UV */
    b[0] = 220; /* scene cut: retain previous, don't dissolve */
    dkr_movie_midframe(out, a, b, 6, 4);
    CHECK(!memcmp(a, out, 6));
    CHECK(dkr_movie_midpoint(0, 33367) == 16683);
    CHECK(dkr_movie_midpoint(33367, 66733) == 50050);
    CHECK(dkr_movie_midpoint(1000000, 0) == -1); /* seek */
    CHECK(dkr_movie_midpoint(0, 16683) == -1); /* already 60 fps */
    CHECK(dkr_movie_midpoint(0, 2000000) == -1); /* gap */
    CHECK(dkr_movie_midpoint(INT64_MIN, INT64_MAX) == -1);
    /* Verify 64-bit SWAR exact rounded average across all 65536 byte pairs and non-multiple-of-8 tail */
    static uint8_t sa[65539], sb[65539], sout[65539];
    for (int i = 0; i < 65536; i++) { sa[i] = (uint8_t)(i >> 8); sb[i] = (uint8_t)(i & 0xff); }
    sa[65536] = 0; sb[65536] = 255;
    sa[65537] = 255; sb[65537] = 255;
    sa[65538] = 17; sb[65538] = 18;
    dkr_movie_midframe(sout, sa, sb, sizeof sa, 0);
    for (int i = 0; i < 65539; i++) CHECK(sout[i] == (uint8_t)((sa[i] + sb[i] + 1) / 2));
    puts("movie interpolation: PASS (NV12, SWAR 64-bit rounding, cut detection, timestamps, seeks)");
    return 0;
}
