/* SDL3 is dynamically loaded from the local toolchain. XInput remains an independent fallback.
 * Only gamepad/joystick subsystems are used: the Vulkan backend owns the window and its message loop.
 * SDL3 API reference: https://wiki.libsdl.org/SDL3/CategoryGamepad (SDL3 itself is zlib licensed). */
#include "dkr/gamepad.h"
#include "dkr/settings.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static int clamp_axis(int value) { return value < -32767 ? -32767 : value > 32767 ? 32767 : value; }

static void stick(int x, int y, unsigned deadzone, int32_t *ox, int32_t *oy) {
    float length = sqrtf((float)x * x + (float)y * y);
    float threshold = (float)(deadzone > 50 ? 50 : deadzone) * 32767.0f / 100.0f;
    if (length <= threshold || length == 0) { *ox = *oy = 0; return; }
    float scaled = (fminf(length, 32767.0f) - threshold) / (32767.0f - threshold) * 32767.0f / length;
    *ox = clamp_axis((int)(x * scaled));
    *oy = clamp_axis((int)(-y * scaled)); /* SDL Y points down; Npad Y points up. */
}

void dkr_gamepad_map(uint32_t buttons, const int16_t axes[6], unsigned deadzone, int swap_face,
                     DkrGamepadState *state) {
    memset(state, 0, sizeof *state);
    state->connected = 1;
    /* SDL: south, east, west, north, back, guide, start, LS, RS, LB, RB, up, down, left, right. */
    static const unsigned npad[] = {1, 0, 3, 2, 11, 31, 10, 4, 5, 6, 7, 13, 15, 12, 14};
    for (unsigned i = 0; i < sizeof npad / sizeof *npad; i++) {
        if (npad[i] == 31 || !(buttons & (1u << i))) continue;
        unsigned bit = npad[i];
        if (swap_face && i < 4) bit ^= 1;
        state->buttons |= 1ull << bit;
    }
    if (axes[4] > 3855) state->buttons |= 1ull << 8;
    if (axes[5] > 3855) state->buttons |= 1ull << 9;
    stick(axes[0], axes[1], deadzone, &state->lx, &state->ly);
    stick(axes[2], axes[3], deadzone, &state->rx, &state->ry);
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Public SDL3 ABI. Keeping this small surface local means the fallback builds without an SDL SDK. */
typedef bool (*InitFn)(uint32_t);
typedef bool (*HintFn)(const char *, const char *);
typedef uint32_t *(*ListFn)(int *);
typedef void *(*OpenFn)(uint32_t);
typedef void (*CloseFn)(void *);
typedef bool (*ConnectedFn)(void *);
typedef bool (*ButtonFn)(void *, int);
typedef int16_t (*AxisFn)(void *, int);
typedef void (*VoidFn)(void);
typedef void (*EventsFn)(bool);
typedef void (*FreeFn)(void *);
typedef const char *(*NameFn)(void *);
typedef bool (*RumbleFn)(void *, uint16_t, uint16_t, uint32_t);
static struct {
    InitFn init; HintFn hint; ListFn list; OpenFn open; CloseFn close; ConnectedFn connected;
    ButtonFn button; AxisFn axis; VoidFn update, quit; EventsFn events; FreeFn free; NameFn name;
    RumbleFn rumble;
} sdl;
static HMODULE library, xinput;
static void *gamepads[2];
static uint32_t instances[2];
static ULONGLONG next_scan;
static int initialized, sdl_ready;
static SRWLOCK g_pad_lock = SRWLOCK_INIT;
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XboxPad;
typedef struct { DWORD packet; XboxPad pad; } XboxState;
typedef struct { WORD left, right; } XboxVibration;
typedef DWORD (WINAPI *XboxFn)(DWORD, XboxState *);
typedef DWORD (WINAPI *XboxSetStateFn)(DWORD, XboxVibration *);
static XboxFn xbox_state;
static XboxSetStateFn xbox_set_state;

void dkr_gamepad_shutdown(void) {
    AcquireSRWLockExclusive(&g_pad_lock);
    for (unsigned p = 0; p < 2; p++) {
        if (gamepads[p]) {
            if (sdl.rumble) sdl.rumble(gamepads[p], 0, 0, 0);
            sdl.close(gamepads[p]);
        }
        gamepads[p] = NULL; instances[p] = 0;
    }
    next_scan = 0;
    if (sdl_ready) sdl.quit();
    sdl_ready = 0;
    if (library) FreeLibrary(library);
    if (xinput) FreeLibrary(xinput);
    library = xinput = NULL;
    xbox_state = NULL;
    xbox_set_state = NULL;
    initialized = 0;
    ReleaseSRWLockExclusive(&g_pad_lock);
}

void dkr_gamepad_init(void) {
    AcquireSRWLockExclusive(&g_pad_lock);
    if (initialized) { ReleaseSRWLockExclusive(&g_pad_lock); return; }
    initialized = 1;
    const DkrSettings *settings = dkr_settings();
    if (settings->input_backend == DKR_INPUT_KEYBOARD) { ReleaseSRWLockExclusive(&g_pad_lock); return; }
    if (settings->input_backend != DKR_INPUT_XINPUT) {
        wchar_t path[32768];
        DWORD count = GetEnvironmentVariableW(L"DKR_SDL3_PATH", path, 32768);
        if (!count || count >= 32768) {
            count = GetModuleFileNameW(NULL, path, 32768);
            if (count && count < 32768) {
                wchar_t *end = wcsrchr(path, L'\\');
                if (end) *end = 0;
                if (wcslen(path) + 40 < 32768) wcscat(path, L"\\..\\..\\toolchain\\sdl3\\SDL3.dll");
            } else path[0] = 0;
        }
        if (*path) library = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (library) {
#define LOAD(field, type, name) sdl.field = (type)GetProcAddress(library, "SDL_" name)
            LOAD(init, InitFn, "Init"); LOAD(hint, HintFn, "SetHint"); LOAD(list, ListFn, "GetGamepads");
            LOAD(open, OpenFn, "OpenGamepad"); LOAD(close, CloseFn, "CloseGamepad");
            LOAD(connected, ConnectedFn, "GamepadConnected"); LOAD(button, ButtonFn, "GetGamepadButton");
            LOAD(axis, AxisFn, "GetGamepadAxis"); LOAD(update, VoidFn, "UpdateGamepads");
            LOAD(quit, VoidFn, "Quit"); LOAD(events, EventsFn, "SetGamepadEventsEnabled");
            LOAD(free, FreeFn, "free"); LOAD(name, NameFn, "GetGamepadName");
            LOAD(rumble, RumbleFn, "RumbleGamepad");
#undef LOAD
            if (sdl.init && sdl.hint && sdl.list && sdl.open && sdl.close && sdl.connected && sdl.button &&
                sdl.axis && sdl.update && sdl.quit && sdl.events && sdl.free && sdl.name) {
                sdl.hint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
                sdl.hint("SDL_JOYSTICK_HIDAPI_PS4", "1");
                sdl.hint("SDL_JOYSTICK_HIDAPI_PS5", "1");
                sdl_ready = sdl.init(0x2000); /* SDL_INIT_GAMEPAD (also initializes joysticks and events). */
                if (sdl_ready) sdl.events(false); /* We poll; do not accumulate an unconsumed event queue. */
            }
        }
        fprintf(stderr, "input: SDL3 %s\n", sdl_ready ? "ready (Xbox / DualShock 4 / DualSense)" : "unavailable; using XInput fallback");
    }
    xinput = LoadLibraryExW(L"xinput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!xinput) xinput = LoadLibraryExW(L"xinput9_1_0.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (xinput) {
        xbox_state = (XboxFn)GetProcAddress(xinput, "XInputGetState");
        xbox_set_state = (XboxSetStateFn)GetProcAddress(xinput, "XInputSetState");
    }
    ReleaseSRWLockExclusive(&g_pad_lock);
    atexit(dkr_gamepad_shutdown);
}

void dkr_gamepad_rumble(unsigned player, float low_amp, float high_amp) {
    if (player >= 2) return;
    if (!(low_amp >= 0.0f)) low_amp = 0.0f;
    if (low_amp > 1.0f) low_amp = 1.0f;
    if (!(high_amp >= 0.0f)) high_amp = 0.0f;
    if (high_amp > 1.0f) high_amp = 1.0f;
    uint16_t lo = (uint16_t)(low_amp * 65535.0f + 0.5f);
    uint16_t hi = (uint16_t)(high_amp * 65535.0f + 0.5f);
    const DkrSettings *settings = dkr_settings();
    AcquireSRWLockExclusive(&g_pad_lock);
    if (initialized && settings->input_backend != DKR_INPUT_KEYBOARD) {
        unsigned indices[2] = {settings->controller_index, settings->second_controller_index};
        if (indices[1] == indices[0]) indices[1] = 4;
        if (sdl_ready) {
            if (gamepads[player] && sdl.rumble)
                sdl.rumble(gamepads[player], lo, hi, (lo || hi) ? 250 : 0);
        } else if (xbox_set_state && indices[player] < 4) {
            XboxVibration vib = {lo, hi};
            xbox_set_state(indices[player], &vib);
        }
    }
    ReleaseSRWLockExclusive(&g_pad_lock);
}

void dkr_gamepad_poll_players(DkrGamepadState states[2]) {
    memset(states, 0, sizeof(*states) * 2);
    const DkrSettings *settings = dkr_settings();
    AcquireSRWLockExclusive(&g_pad_lock);
    if (!initialized || settings->input_backend == DKR_INPUT_KEYBOARD) {
        ReleaseSRWLockExclusive(&g_pad_lock);
        return;
    }
    unsigned indices[2] = {settings->controller_index, settings->second_controller_index};
    if (indices[1] == indices[0]) indices[1] = 4; /* never assign one device to both players */
    if (sdl_ready) {
        sdl.update();
        for (unsigned p = 0; p < 2; p++) {
            if (gamepads[p] && !sdl.connected(gamepads[p])) {
                fprintf(stderr, "input: player %u disconnected\n", p + 1);
                sdl.close(gamepads[p]); gamepads[p] = NULL; instances[p] = 0; next_scan = 0;
            }
        }
        if (GetTickCount64() >= next_scan) {
            next_scan = GetTickCount64() + 500;
            int count = 0;
            uint32_t *ids = sdl.list(&count);
            for (unsigned p = 0; p < 2; p++) {
                if (gamepads[p] || indices[p] >= 4) continue;
                uint32_t wanted = ids && indices[p] < (unsigned)count ? ids[indices[p]] : 0;
                /* SDL compacts its list on unplug. Keep the surviving player's device and
                 * claim the next unowned device when the missing player reconnects. */
                if (wanted && wanted == instances[1 - p]) {
                    wanted = 0;
                    for (unsigned i = indices[p]; ids && i < (unsigned)count; i++)
                        if (ids[i] != instances[1 - p]) { wanted = ids[i]; break; }
                }
                if (wanted && wanted != instances[1 - p]) {
                    gamepads[p] = sdl.open(wanted);
                    instances[p] = gamepads[p] ? wanted : 0;
                    if (gamepads[p]) fprintf(stderr, "input: connected %s (SDL3, player %u)\n", sdl.name(gamepads[p]), p + 1);
                }
            }
            if (ids) sdl.free(ids);
        }
        for (unsigned p = 0; p < 2; p++) {
            if (!gamepads[p]) continue;
            uint32_t buttons = 0;
            int16_t axes[6];
            for (int i = 0; i < 15; i++) if (sdl.button(gamepads[p], i)) buttons |= 1u << i;
            for (int i = 0; i < 6; i++) axes[i] = sdl.axis(gamepads[p], i);
            dkr_gamepad_map(buttons, axes, settings->stick_deadzone, settings->swap_abxy, &states[p]);
        }
        ReleaseSRWLockExclusive(&g_pad_lock);
        return; /* SDL owns device selection; do not also merge XInput. */
    }
    if (!xbox_state) {
        ReleaseSRWLockExclusive(&g_pad_lock);
        return;
    }
    for (unsigned p = 0; p < 2; p++) {
        if (indices[p] >= 4) continue;
        XboxState xbox;
        if (xbox_state(indices[p], &xbox)) continue;
        static const unsigned xbox_bits[] = {0x1000, 0x2000, 0x4000, 0x8000, 0x20, 0, 0x10, 0x40, 0x80,
                                            0x100, 0x200, 1, 2, 4, 8};
        uint32_t buttons = 0;
        for (unsigned i = 0; i < sizeof xbox_bits / sizeof *xbox_bits; i++) if (xbox.pad.buttons & xbox_bits[i]) buttons |= 1u << i;
        int16_t axes[] = {xbox.pad.lx, (int16_t)-clamp_axis(xbox.pad.ly), xbox.pad.rx,
                         (int16_t)-clamp_axis(xbox.pad.ry), (int16_t)(xbox.pad.lt * 32767 / 255), (int16_t)(xbox.pad.rt * 32767 / 255)};
        dkr_gamepad_map(buttons, axes, settings->stick_deadzone, settings->swap_abxy, &states[p]);
    }
    ReleaseSRWLockExclusive(&g_pad_lock);
}
#else
void dkr_gamepad_init(void) {}
void dkr_gamepad_shutdown(void) {}
void dkr_gamepad_rumble(unsigned player, float low_amp, float high_amp) { (void)player; (void)low_amp; (void)high_amp; }
void dkr_gamepad_poll_players(DkrGamepadState states[2]) { memset(states, 0, sizeof(*states) * 2); }
#endif

int dkr_gamepad_poll(DkrGamepadState *state) {
    DkrGamepadState states[2];
    dkr_gamepad_poll_players(states);
    *state = states[0];
    return state->connected;
}
