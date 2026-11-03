/* Host graphics entry points. The Vulkan backend owns its window, device and presentation queue.
 * Guest graphics adapters will use this backend; it has no dependency on a game's CPU state or assets. */
#ifndef DKR_GFX_H
#define DKR_GFX_H

#include <stddef.h>
#include <stdint.h>

typedef struct DkrGfx DkrGfx;
typedef struct DkrGfxConfig {
    uint32_t width, height;
    const char *title;
    int visible;
    int validation; /* -1: use the validation layer if installed, 0: off, 1: require it */
    int borderless; /* start as a borderless window covering the monitor (Alt+Enter or F11 toggles) */
    int vsync;      /* 0: FIFO, 1: mailbox when available, 2: immediate when available */
    int linear;     /* present with linear filtering instead of nearest */
    int keep_aspect;/* letterbox the presented image to its own aspect ratio */
} DkrGfxConfig;

/* Functions returning an int use 0 for success and -1 for error, except poll (1 open, 0 closed).
 * Render/poll/resize calls belong to the window's creating thread. The error string stays valid until destroy. */
/* Create returns a partially initialized handle on failure; inspect its error and destroy it as usual. */
int dkr_gfx_create(DkrGfx **out, const DkrGfxConfig *config);
int dkr_gfx_poll(DkrGfx *gfx);
int dkr_gfx_resize(DkrGfx *gfx, uint32_t width, uint32_t height);
int dkr_gfx_clear_present(DkrGfx *gfx, const float color[4], const char *capture_bmp);
const char *dkr_gfx_error(const DkrGfx *gfx);
/* Any thread: the window thread applies it. */
void dkr_gfx_set_title(DkrGfx *gfx, const char *utf8);
uint64_t dkr_gfx_frames(const DkrGfx *gfx);
uint32_t dkr_gfx_validation_errors(const DkrGfx *gfx);
/* Returns the validation error count after teardown, including errors emitted while destroying resources. */
uint32_t dkr_gfx_destroy(DkrGfx *gfx);

#endif
