/* Real Vulkan/WSI smoke test, independent of game files. Read back the actual swapchain pixels and check the
 * complete capture after resizing; a successful API return alone is not enough to pass. */
#include "dkr/gfx.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check_capture(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) { fprintf(stderr, "cannot read the capture\n"); return -1; }
    BITMAPFILEHEADER header;
    BITMAPINFOHEADER info;
    int ok = fread(&header, 1, sizeof header, file) == sizeof header &&
             fread(&info, 1, sizeof info, file) == sizeof info;
    ok = ok && header.bfType == 0x4d42 && info.biWidth == 320 && info.biHeight == -240 && info.biBitCount == 32;
    unsigned pixels = 0;
    if (ok) {
        fseek(file, (long)header.bfOffBits, SEEK_SET);
        for (unsigned i = 0; i < 320 * 240; i++) {
            unsigned char pixel[4];
            if (fread(pixel, 1, 4, file) != 4 || abs((int)pixel[0] - 191) > 1 ||
                abs((int)pixel[1] - 128) > 1 || abs((int)pixel[2] - 64) > 1 || pixel[3] != 255) {
                ok = 0;
                fprintf(stderr, "unexpected GPU pixel at index %u\n", i);
                break;
            }
            pixels++;
        }
    }
    fclose(file);
    if (ok) printf("capture: %u GPU pixels match RGBA(0.25, 0.5, 0.75, 1), 320x240\n", pixels);
    return ok ? 0 : -1;
}

int main(int argc, char **argv) {
    int visible = 1, validation = -1, hold = 0;
    uint64_t frames = 8;
    const char *capture = "gfx-smoke.bmp";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hidden")) visible = 0;
        else if (!strcmp(argv[i], "--validation")) validation = 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capture = argv[++i];
        else if (!strcmp(argv[i], "--hold-seconds") && i + 1 < argc) hold = atoi(argv[++i]);
        else { fprintf(stderr, "usage: gfx_smoke [--hidden] [--validation] [--frames N>=4] [--capture PATH] [--hold-seconds N]\n"); return 2; }
    }
    if (frames < 4 || frames > 36000 || hold < 0 || hold > 60) return 2;
    DkrGfx *gfx = NULL;
    DkrGfxConfig config = {640, 360, "DKRecomp - Vulkan smoke test", visible, validation};
    if (dkr_gfx_create(&gfx, &config)) {
        fprintf(stderr, "FAIL: %s\n", dkr_gfx_error(gfx));
        dkr_gfx_destroy(gfx);
        return 1;
    }
    const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    int resized = 0, failed = 0, captured = 0;
    ULONGLONG deadline = GetTickCount64() + 20000;
    while (dkr_gfx_frames(gfx) < frames && dkr_gfx_poll(gfx)) {
        if (GetTickCount64() > deadline) { failed = 1; fprintf(stderr, "FAIL: presentation timed out\n"); break; }
        uint64_t current = dkr_gfx_frames(gfx);
        if (!resized && current >= frames / 2) {
            if (dkr_gfx_resize(gfx, 320, 240)) { failed = 1; break; }
            resized = 1;
        }
        const char *target = current + 1 == frames ? capture : NULL;
        if (dkr_gfx_clear_present(gfx, color, target)) { failed = 1; break; }
        if (target && dkr_gfx_frames(gfx) > current) captured = 1;
        if (dkr_gfx_frames(gfx) == current) Sleep(1);
    }
    uint64_t presented = dkr_gfx_frames(gfx);
    uint32_t errors = dkr_gfx_validation_errors(gfx);
    if (failed) fprintf(stderr, "FAIL: %s\n", dkr_gfx_error(gfx));
    failed |= presented != frames || !resized || !captured || errors != 0;
    if (!failed) failed = check_capture(capture) != 0;
    if (!failed && hold) {
        ULONGLONG close_at = GetTickCount64() + (ULONGLONG)hold * 1000;
        while (dkr_gfx_poll(gfx) && GetTickCount64() < close_at) Sleep(10);
    }
    errors = dkr_gfx_destroy(gfx);
    failed |= errors != 0;
    printf("%s: %llu presents, resized=%d, validation_errors=%u\n", failed ? "FAIL" : "PASS",
        (unsigned long long)presented, resized, errors);
    return failed != 0;
}
