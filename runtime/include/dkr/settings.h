/* User settings: config.local.toml at the repository root (written by the launcher, ignored by Git), then
 * DKR_<KEY> environment variables, which win. Unknown keys are reported and ignored; bad values keep the default. */
#ifndef DKR_SETTINGS_H
#define DKR_SETTINGS_H

typedef enum { DKR_DISPLAY_WINDOWED, DKR_DISPLAY_BORDERLESS } DkrDisplayMode;
typedef enum { DKR_VSYNC_ON, DKR_VSYNC_MAILBOX, DKR_VSYNC_OFF } DkrVsync;
typedef enum { DKR_INPUT_AUTO, DKR_INPUT_XINPUT, DKR_INPUT_KEYBOARD } DkrInputBackend;

typedef struct DkrSettings {
    DkrDisplayMode display_mode;   /* display_mode = "windowed" | "borderless" (full screen without a border) */
    unsigned window_width, window_height; /* window client size; 0 = 75% of the monitor, 16:9 */
    float render_scale;            /* render_scale = 1.0 .. 4.0: the game's 1920x1080 targets are rendered this much larger */
    DkrVsync vsync;                /* vsync = "on" (FIFO) | "mailbox" (no tearing, low latency) | "off" (immediate) */
    unsigned fps_limit;            /* fps_limit = 30 | 60 | 0 (no host pacing beyond the game's own) */
    int linear_filter;             /* present_filter = "linear" | "nearest" */
    int keep_aspect;               /* keep_aspect = true: letterbox instead of stretching to the window */
    int show_stats;                /* show_stats = true: frame rate in the window title and DKR_PERF statistics */
    int mute;                      /* mute = true: process audio without a Windows output device */
    int movie_smoothing;           /* optional timestamp-preserving NV12 midframes, ~59.94 fps */
    DkrInputBackend input_backend;
    unsigned controller_index, second_controller_index, stick_deadzone; /* second: 4 disables player 2 */
    int swap_abxy, input_background;
} DkrSettings;

/* Loads once (later calls return the same values). `path` may be NULL to read only the environment. */
const DkrSettings *dkr_settings_load(const char *path);
const DkrSettings *dkr_settings(void);
#endif
