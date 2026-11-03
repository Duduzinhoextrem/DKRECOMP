/* Known-answer tests for Xbox/PlayStation physical positions, Npad axes and radial deadzones. */
#include "dkr/gamepad.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void) {
    DkrGamepadState pad;
    int16_t axes[6] = {0};
    const unsigned target[] = {1, 0, 3, 2, 11, 31, 10, 4, 5, 6, 7, 13, 15, 12, 14};
    for (unsigned i = 0; i < 15; i++) {
        dkr_gamepad_map(1u << i, axes, 15, 0, &pad);
        CHECK(pad.buttons == (target[i] == 31 ? 0 : 1ull << target[i]));
        if (i < 4) {
            dkr_gamepad_map(1u << i, axes, 15, 1, &pad);
            CHECK(pad.buttons == (1ull << (target[i] ^ 1)));
        }
    }
    axes[0] = 2000; axes[1] = -3000;
    dkr_gamepad_map(0, axes, 15, 0, &pad);
    CHECK(pad.lx == 0 && pad.ly == 0);
    axes[0] = 32767; axes[1] = 0; axes[2] = 0; axes[3] = -32768;
    axes[4] = 3855; axes[5] = 3856;
    dkr_gamepad_map(0, axes, 15, 0, &pad);
    CHECK(abs(pad.lx - 32767) <= 1 && pad.ly == 0 && abs(pad.ry - 32767) <= 1);
    CHECK(pad.buttons == (1ull << 9));
    axes[0] = -32768; axes[1] = 32767; axes[4] = 32767;
    dkr_gamepad_map(0, axes, 15, 0, &pad);
    CHECK(pad.lx < -23000 && pad.ly < -23000 && pad.lx >= -32767 && pad.ly >= -32767);
    CHECK(pad.buttons == ((1ull << 8) | (1ull << 9)));
    axes[0] = 16000; axes[1] = 0;
    dkr_gamepad_map(0, axes, 0, 0, &pad);
    CHECK(abs(pad.lx - 16000) <= 1);
    dkr_gamepad_map(0, axes, 50, 0, &pad);
    CHECK(pad.lx == 0);
    puts("gamepad mapping: PASS (Xbox / PlayStation, all positions, triggers, Y polarity, deadzone, swaps)");
    return 0;
}
