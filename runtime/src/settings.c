/* User settings (see dkr/settings.h). The file is a small subset of TOML: `key = value` lines, `# comments` and
 * `[section]` headers, which only group keys for people and are otherwise ignored. Strings may be quoted. */
#include "dkr/settings.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DkrSettings g_settings = {
    .display_mode = DKR_DISPLAY_WINDOWED, .window_width = 0, .window_height = 0, .render_scale = 1.0f,
    .vsync = DKR_VSYNC_MAILBOX, .fps_limit = 60, .linear_filter = 1, .keep_aspect = 1, .show_stats = 0, .mute = 0,
    .movie_smoothing = 1, .input_backend = DKR_INPUT_AUTO, .controller_index = 0, .second_controller_index = 1,
    .stick_deadzone = 15, .swap_abxy = 0, .input_background = 0,
};
static int g_loaded;

static int truthy(const char *v) { return !strcmp(v, "true") || !strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "on"); }

static void apply(const char *key, const char *value, const char *origin) {
    DkrSettings *s = &g_settings;
    char *end;
    if (!strcmp(key, "display_mode")) {
        if (!strcmp(value, "windowed")) s->display_mode = DKR_DISPLAY_WINDOWED;
        else if (!strcmp(value, "borderless") || !strcmp(value, "fullscreen")) s->display_mode = DKR_DISPLAY_BORDERLESS;
        else goto bad;
    } else if (!strcmp(key, "window_width") || !strcmp(key, "window_height")) {
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end || (v && (v < 320 || v > 16384))) goto bad;
        if (key[7] == 'w') s->window_width = (unsigned)v; else s->window_height = (unsigned)v;
    } else if (!strcmp(key, "render_scale")) {
        float v = strtof(value, &end);
        if (end == value || *end || !(v >= 0.5f && v <= 4.0f)) goto bad;
        s->render_scale = v;
    } else if (!strcmp(key, "vsync")) {
        if (!strcmp(value, "on") || !strcmp(value, "fifo") || !strcmp(value, "true")) s->vsync = DKR_VSYNC_ON;
        else if (!strcmp(value, "mailbox")) s->vsync = DKR_VSYNC_MAILBOX;
        else if (!strcmp(value, "off") || !strcmp(value, "false") || !strcmp(value, "immediate")) s->vsync = DKR_VSYNC_OFF;
        else goto bad;
    } else if (!strcmp(key, "fps_limit")) {
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end || v > 1000) goto bad;
        s->fps_limit = (unsigned)v;
    } else if (!strcmp(key, "present_filter")) {
        if (!strcmp(value, "linear")) s->linear_filter = 1;
        else if (!strcmp(value, "nearest")) s->linear_filter = 0;
        else goto bad;
    } else if (!strcmp(key, "input_backend")) {
        if (!strcmp(value, "auto")) s->input_backend = DKR_INPUT_AUTO;
        else if (!strcmp(value, "xinput")) s->input_backend = DKR_INPUT_XINPUT;
        else if (!strcmp(value, "keyboard")) s->input_backend = DKR_INPUT_KEYBOARD;
        else goto bad;
    } else if (!strcmp(key, "controller_index") || !strcmp(key, "second_controller_index") || !strcmp(key, "stick_deadzone")) {
        unsigned long v = strtoul(value, &end, 10);
        if (end == value || *end || v > (!strcmp(key, "controller_index") ? 3 : !strcmp(key, "second_controller_index") ? 4 : 50)) goto bad;
        if (!strcmp(key, "controller_index")) s->controller_index = (unsigned)v;
        else if (!strcmp(key, "second_controller_index")) s->second_controller_index = (unsigned)v;
        else s->stick_deadzone = (unsigned)v;
    } else {
        int *boolean = !strcmp(key, "keep_aspect") ? &s->keep_aspect : !strcmp(key, "show_stats") ? &s->show_stats :
            !strcmp(key, "mute") ? &s->mute : !strcmp(key, "movie_smoothing") ? &s->movie_smoothing : !strcmp(key, "swap_abxy") ? &s->swap_abxy :
            !strcmp(key, "input_background") ? &s->input_background : NULL;
        if (boolean) {
            if (truthy(value)) *boolean = 1;
            else if (!strcmp(value, "false") || !strcmp(value, "0") || !strcmp(value, "no") || !strcmp(value, "off")) *boolean = 0;
            else goto bad;
        } else fprintf(stderr, "settings: unknown key '%s' in %s ignored\n", key, origin);
    }
    return;
bad:
    fprintf(stderr, "settings: invalid value '%s' for %s in %s; keeping the default\n", value, key, origin);
}

static char *trim(char *p) {
    while (isspace((unsigned char)*p)) p++;
    char *e = p + strlen(p);
    while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
    if (e - p >= 2 && (*p == '"' || *p == '\'') && e[-1] == *p) { e[-1] = 0; p++; }
    return p;
}

const DkrSettings *dkr_settings_load(const char *path) {
    if (g_loaded) return &g_settings;
    g_loaded = 1;
    FILE *f = path ? fopen(path, "r") : NULL;
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char *hash = NULL;
            int quoted = 0;
            for (char *p = line; *p; p++) {
                if (*p == '"') quoted = !quoted;
                if (*p == '#' && !quoted) { hash = p; break; }
            }
            if (hash) *hash = 0;
            char *p = trim(line);
            if (!*p || *p == '[') continue;
            char *eq = strchr(p, '=');
            if (!eq) { fprintf(stderr, "settings: line without '=' in %s ignored\n", path); continue; }
            *eq = 0;
            apply(trim(p), trim(eq + 1), path);
        }
        fclose(f);
    }
    static const char *keys[] = {"display_mode", "window_width", "window_height", "render_scale", "vsync", "fps_limit",
                                 "present_filter", "keep_aspect", "show_stats", "mute", "input_backend",
                                 "controller_index", "second_controller_index", "stick_deadzone", "swap_abxy", "input_background", "movie_smoothing"};
    for (unsigned i = 0; i < sizeof keys / sizeof *keys; i++) {
        char name[64] = "DKR_";
        for (unsigned k = 0; keys[i][k] && k < 58; k++) name[4 + k] = (char)toupper((unsigned char)keys[i][k]);
        const char *v = getenv(name);
        if (v && *v) apply(keys[i], v, "the environment");
    }
    /* Older switches keep working. */
    if (getenv("DKR_PERF") && strcmp(getenv("DKR_PERF"), "0")) g_settings.show_stats = 1;
    if (getenv("DKR_AUDIO_NULL")) g_settings.mute = 1;
    return &g_settings;
}

const DkrSettings *dkr_settings(void) { return dkr_settings_load(NULL); }
