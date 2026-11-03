/* Malformed configuration must retain safe defaults; valid environment overrides still win. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "dkr/settings.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void) {
    const char *clear[] = {"DISPLAY_MODE", "WINDOW_WIDTH", "WINDOW_HEIGHT", "RENDER_SCALE", "VSYNC", "FPS_LIMIT", "PRESENT_FILTER",
        "KEEP_ASPECT", "SHOW_STATS", "MUTE", "MOVIE_SMOOTHING", "INPUT_BACKEND", "CONTROLLER_INDEX", "SECOND_CONTROLLER_INDEX", "STICK_DEADZONE", "SWAP_ABXY", "INPUT_BACKGROUND", "PERF", "AUDIO_NULL"};
    for (unsigned i = 0; i < sizeof clear / sizeof *clear; i++) {
        char name[64]; snprintf(name, sizeof name, "DKR_%s", clear[i]); _putenv_s(name, "");
    }
    char path[128]; snprintf(path, sizeof path, "settings-test-%lu.local.toml", GetCurrentProcessId());
    FILE *file = fopen(path, "w"); CHECK(file != NULL);
    fputs("fps_limit =\nrender_scale = nan\nkeep_aspect = maybe\ncontroller_index = 4\n"
          "input_backend = invalid\nshow_stats = true\nwindow_height = 720\n", file);
    fclose(file);
    _putenv_s("DKR_WINDOW_WIDTH", "1366"); _putenv_s("DKR_INPUT_BACKEND", "xinput"); _putenv_s("DKR_STICK_DEADZONE", "18");
    const DkrSettings *s = dkr_settings_load(path);
    remove(path);
    CHECK(s->fps_limit == 60 && s->render_scale == 1 && s->keep_aspect == 1 && s->controller_index == 0);
    CHECK(s->window_width == 1366 && s->window_height == 720 && s->input_backend == DKR_INPUT_XINPUT);
    CHECK(s->stick_deadzone == 18 && s->show_stats == 1 && s->second_controller_index == 1 && s->movie_smoothing == 1);
    CHECK(s == dkr_settings());
    puts("settings: PASS (invalid values, safe defaults, environment precedence, cached configuration)");
    return 0;
}
