/* Initial HID service: the applet resource owns the service's 256 KiB input shared memory.
 * Command/layout reference: switchbrew HID services, firmware 4.x. Initially all controllers are disconnected.
 * Host input (keyboard + SDL3 / XInput) polls asynchronously and updates shared memory Npad rings at ~200 Hz. */
#include "hos.h"
#include "dkr/gamepad.h"
#include "dkr/settings.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define DKR_BARRIER() MemoryBarrier()
static SRWLOCK g_hid_lock = SRWLOCK_INIT;
#else
#define DKR_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif

/* Firmware 4.x Npad Shared Memory Structures */
typedef struct NpadCommonState {
    uint64_t sampling_number;
    uint64_t buttons;
    int32_t stick_l_x;
    int32_t stick_l_y;
    int32_t stick_r_x;
    int32_t stick_r_y;
    uint32_t attributes;
    uint32_t reserved;
} NpadCommonState;

typedef struct NpadCommonStateAtomicStorage {
    uint64_t sampling_number;
    NpadCommonState state;
} NpadCommonStateAtomicStorage;

typedef struct NpadCommonLifo {
    uint64_t unused;
    uint64_t buffer_count;
    uint64_t tail;
    uint64_t count;
    NpadCommonStateAtomicStorage storage[17];
} NpadCommonLifo;

_Static_assert(sizeof(NpadCommonState) == 0x28, "NpadCommonState size ABI");
_Static_assert(sizeof(NpadCommonStateAtomicStorage) == 0x30, "NpadCommonStateAtomicStorage size ABI");
_Static_assert(sizeof(NpadCommonLifo) == 0x350, "NpadCommonLifo size ABI");

typedef struct SixAxisSensorState {
    uint64_t sampling_number;
    uint64_t delta_time_ns;
    float accel_x, accel_y, accel_z;
    float gyro_x, gyro_y, gyro_z;
    float angle_x, angle_y, angle_z;
    float dir_matrix[9];
    uint32_t attributes;
    uint32_t reserved;
} SixAxisSensorState;

typedef struct SixAxisSensorStateAtomicStorage {
    uint64_t sampling_number;
    SixAxisSensorState state;
} SixAxisSensorStateAtomicStorage;

typedef struct SixAxisSensorLifo {
    uint64_t unused;
    uint64_t buffer_count;
    uint64_t tail;
    uint64_t count;
    SixAxisSensorStateAtomicStorage storage[17];
} SixAxisSensorLifo;

_Static_assert(sizeof(SixAxisSensorState) == 0x60, "SixAxisSensorState size ABI");
_Static_assert(sizeof(SixAxisSensorStateAtomicStorage) == 0x68, "SixAxisSensorStateAtomicStorage size ABI");
_Static_assert(sizeof(SixAxisSensorLifo) == 0x708, "SixAxisSensorLifo size ABI");

static uint32_t g_memory;
static volatile int g_npad_active;
static uint64_t g_joy_hold_type;
static uint64_t g_handheld_mode;
static uint32_t g_supported_styles, g_supported_ids = 0x3ff;
static uint32_t g_style_events[10];
static int g_vibration_permitted = 1;
/* Last VibrationValue (amp_low, freq_low, amp_high, freq_high) per VibrationDeviceHandle. A handle is
 * byte 0 device type, byte 1 NpadIdType (0..7 = No1..No8, 0x20 = Handheld), byte 2 device index (0 left, 1 right).
 * The game drives up to 18 of them; neutral values sent to disconnected pads must not reach player 1's motors. */
typedef struct VibrationDevice { uint32_t handle; float value[4]; } VibrationDevice;
static VibrationDevice g_vibration_devices[64];
static unsigned g_vibration_device_count;
static const float g_vibration_neutral[4] = {0.f, 160.f, 0.f, 320.f};
static uint64_t g_sampling_number = 0;

static int g_test_input_override = 0;
static DkrGamepadState g_test_players[2];

void hos_hid_set_test_player_input(unsigned player, int connected, uint64_t buttons,
                                  int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    if (player >= 2) return;
    g_test_input_override = 1;
    g_test_players[player] = (DkrGamepadState){buttons, lx, ly, rx, ry, connected};
}
void hos_hid_set_test_input(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    hos_hid_set_test_player_input(0, 1, buttons, lx, ly, rx, ry);
}
void hos_hid_clear_test_input(void) {
    g_test_input_override = 0;
    memset(g_test_players, 0, sizeof g_test_players);
}

typedef struct VibrationList {
    uint32_t handles[128];
    unsigned count;
} VibrationList;

static void vibration_list_destroy(Service *s) { free(s->state); free(s); }

static void vibration_list_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    if (rq->cmd != 0) hos_unimplemented(c, s, rq);
    if (rq->args_size < 4) { rp->result = HOS_KERNEL(103); return; }
    VibrationList *list = s->state;
    uint32_t handle = RQ_GET(rq, uint32_t, 0);
    for (unsigned i = 0; i < list->count; i++) if (list->handles[i] == handle) return;
    if (list->count == sizeof list->handles / sizeof *list->handles) { rp->result = HOS_KERNEL(105); return; }
    /* This activates a device in the client's resource list; it does not claim a host rumble operation. */
    list->handles[list->count++] = handle;
}

/* Host player of a vibration handle: No1 and Handheld are player 1, No2 is player 2, other pads have no motor. */
static int vibration_player(uint32_t handle) {
    uint32_t npad = (handle >> 8) & 0xff;
    return npad == 0 || npad == 0x20 ? 0 : npad == 1 ? 1 : -1;
}
static float vibration_amplitude(float value) { return value >= 0.f ? (value <= 1.f ? value : 1.f) : 0.f; } /* NaN -> 0 */

static VibrationDevice *vibration_device(uint32_t handle, int create) {
    for (unsigned i = 0; i < g_vibration_device_count; i++)
        if (g_vibration_devices[i].handle == handle) return &g_vibration_devices[i];
    if (!create || g_vibration_device_count == sizeof g_vibration_devices / sizeof *g_vibration_devices) return NULL;
    VibrationDevice *d = &g_vibration_devices[g_vibration_device_count++];
    d->handle = handle;
    memcpy(d->value, g_vibration_neutral, sizeof d->value);
    return d;
}

/* The strongest low/high band over all of a player's motors (left and right Joy-Con, or the Handheld pair). */
void hos_hid_vibration_amplitude(unsigned player, float *low, float *high) {
    *low = *high = 0.f;
    if (!g_vibration_permitted) return;
    for (unsigned i = 0; i < g_vibration_device_count; i++) {
        const VibrationDevice *d = &g_vibration_devices[i];
        if (vibration_player(d->handle) != (int)player) continue;
        float a = vibration_amplitude(d->value[0]), b = vibration_amplitude(d->value[2]);
        if (a > *low) *low = a;
        if (b > *high) *high = b;
    }
}

/* Non-zero values are re-sent on every update because SDL rumble expires on its own after a short time. */
static void vibration_apply(unsigned player) {
    float low, high;
    hos_hid_vibration_amplitude(player, &low, &high);
    dkr_gamepad_rumble(player, low, high);
}

static void vibration_send(uint32_t handle, const void *value) {
    VibrationDevice *d = vibration_device(handle, 1);
    if (d) memcpy(d->value, value, sizeof d->value);
    int player = vibration_player(handle);
    if (player >= 0) vibration_apply((unsigned)player);
}

static int npad_index(uint32_t id) {
    return id < 8 ? (int)id : id == 0x20 ? 8 : id == 0x10 ? 9 : -1;
}

static void npad_lifo_write(NpadCommonLifo *lifo, uint64_t sample, uint64_t buttons,
                            int32_t lx, int32_t ly, int32_t rx, int32_t ry, uint32_t attributes) {
    uint64_t tail = lifo->tail;
    uint64_t count = lifo->count;
    uint64_t next_tail;
    uint64_t next_count;
    if (count == 0) {
        next_tail = 0;
        next_count = 1;
    } else {
        next_tail = (tail + 1) % 17;
        next_count = count < 17 ? count + 1 : 17;
    }

    NpadCommonStateAtomicStorage *entry = &lifo->storage[next_tail];
    entry->sampling_number = 0;
    DKR_BARRIER();

    entry->state.sampling_number = sample;
    entry->state.buttons = buttons;
    entry->state.stick_l_x = lx;
    entry->state.stick_l_y = ly;
    entry->state.stick_r_x = rx;
    entry->state.stick_r_y = ry;
    entry->state.attributes = attributes;
    entry->state.reserved = 0;

    DKR_BARRIER();
    entry->sampling_number = sample;
    DKR_BARRIER();

    lifo->tail = next_tail;
    lifo->count = next_count;
}

static void sixaxis_lifo_write(SixAxisSensorLifo *lifo, uint64_t sample) {
    uint64_t tail = lifo->tail;
    uint64_t count = lifo->count;
    uint64_t next_tail;
    uint64_t next_count;
    if (count == 0) {
        next_tail = 0;
        next_count = 1;
    } else {
        next_tail = (tail + 1) % 17;
        next_count = count < 17 ? count + 1 : 17;
    }

    SixAxisSensorStateAtomicStorage *entry = &lifo->storage[next_tail];
    entry->sampling_number = 0;
    DKR_BARRIER();

    entry->state.sampling_number = sample;
    entry->state.delta_time_ns = 5000000; /* 5 ms (200 Hz) */
    entry->state.accel_x = 0.0f;
    entry->state.accel_y = 0.0f;
    entry->state.accel_z = 1.0f; /* 1G normal gravity */
    entry->state.gyro_x = 0.0f;
    entry->state.gyro_y = 0.0f;
    entry->state.gyro_z = 0.0f;
    entry->state.angle_x = 0.0f;
    entry->state.angle_y = 0.0f;
    entry->state.angle_z = 0.0f;
    memset(entry->state.dir_matrix, 0, sizeof entry->state.dir_matrix);
    entry->state.dir_matrix[0] = 1.0f;
    entry->state.dir_matrix[4] = 1.0f;
    entry->state.dir_matrix[8] = 1.0f;
    entry->state.attributes = 1; /* IsConnected */
    entry->state.reserved = 0;

    DKR_BARRIER();
    entry->sampling_number = sample;
    DKR_BARRIER();

    lifo->tail = next_tail;
    lifo->count = next_count;
}

#ifdef _WIN32
typedef SHORT (WINAPI *GetAsyncKeyState_Fn)(int vKey);
static GetAsyncKeyState_Fn pGetAsyncKeyState = NULL;
static int g_user32_loaded = 0;

static void load_user32(void) {
    if (g_user32_loaded) return;
    g_user32_loaded = 1;
    HMODULE h = LoadLibraryA("user32.dll");
    if (h) {
        pGetAsyncKeyState = (GetAsyncKeyState_Fn)GetProcAddress(h, "GetAsyncKeyState");
    }
}

/* GetAsyncKeyState reads the global keyboard: without this check, typing in any other window drove the game. */
static int game_has_focus(void) {
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

/* DKR_INPUT_SCRIPT="start-end:BUTTON+BUTTON;..." holds buttons between two times in seconds since the first input
 * poll, for unattended runs (e.g. "8-8.3:L+R;12-12.2:A"). Names: A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN. */
typedef struct ScriptStep { double start, end; uint64_t buttons; } ScriptStep;
static ScriptStep g_script[64];
static int g_script_count = -1;
static LARGE_INTEGER g_script_origin;

static uint64_t script_buttons(void) {
    if (g_script_count < 0) {
        g_script_count = 0;
        QueryPerformanceCounter(&g_script_origin);
        const char *text = getenv("DKR_INPUT_SCRIPT");
        static const struct { const char *name; int bit; } names[] = {{"A", 0}, {"B", 1}, {"X", 2}, {"Y", 3},
            {"L", 6}, {"R", 7}, {"ZL", 8}, {"ZR", 9}, {"PLUS", 10}, {"MINUS", 11}, {"LEFT", 12}, {"UP", 13},
            {"RIGHT", 14}, {"DOWN", 15}};
        while (text && *text && g_script_count < 64) {
            ScriptStep step = {0};
            char *end;
            step.start = strtod(text, &end);
            if (*end != '-') break;
            step.end = strtod(end + 1, &end);
            if (*end != ':') break;
            text = end + 1;
            while (*text && *text != ';') {
                size_t n = strcspn(text, "+;");
                for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
                    if (strlen(names[i].name) == n && !_strnicmp(text, names[i].name, n)) step.buttons |= 1ull << names[i].bit;
                text += n;
                if (*text == '+') text++;
            }
            if (*text == ';') text++;
            g_script[g_script_count++] = step;
        }
        if (g_script_count) hos_log("hid: input script with %d steps\n", g_script_count);
    }
    if (!g_script_count) return 0;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
    double t = (double)(now.QuadPart - g_script_origin.QuadPart) / (double)freq.QuadPart;
    uint64_t buttons = 0;
    for (int i = 0; i < g_script_count; i++) if (t >= g_script[i].start && t < g_script[i].end) buttons |= g_script[i].buttons;
    return buttons;
}

static void poll_keyboard(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    (void)rx; (void)ry;
    if (!g_user32_loaded) load_user32();
    if (!pGetAsyncKeyState || !game_has_focus()) return;

    #define KEY_DOWN(k) ((pGetAsyncKeyState(k) & 0x8000) != 0)

    // Left stick (WASD)
    int w = KEY_DOWN('W'), s = KEY_DOWN('S');
    int a = KEY_DOWN('A'), d = KEY_DOWN('D');
    if (w && !s) *ly = 32767;
    else if (s && !w) *ly = -32767;
    if (d && !a) *lx = 32767;
    else if (a && !d) *lx = -32767;

    // D-Pad (Arrows)
    if (KEY_DOWN(VK_LEFT))  *buttons |= (1ull << 12);
    if (KEY_DOWN(VK_UP))    *buttons |= (1ull << 13);
    if (KEY_DOWN(VK_RIGHT)) *buttons |= (1ull << 14);
    if (KEY_DOWN(VK_DOWN))  *buttons |= (1ull << 15);

    // If WASD is not deflecting stick, Arrow keys also deflect Left Stick
    if (*lx == 0) {
        if (*buttons & (1ull << 12)) *lx = -32767;
        else if (*buttons & (1ull << 14)) *lx = 32767;
    }
    if (*ly == 0) {
        if (*buttons & (1ull << 13)) *ly = 32767;
        else if (*buttons & (1ull << 15)) *ly = -32767;
    }

    // Action buttons
    if (KEY_DOWN('L') || KEY_DOWN('E')) *buttons |= (1ull << 0); // A
    if (KEY_DOWN(VK_SPACE) || KEY_DOWN('K')) *buttons |= (1ull << 1); // B
    if (KEY_DOWN('I')) *buttons |= (1ull << 2); // X
    if (KEY_DOWN('J') || KEY_DOWN('Q')) *buttons |= (1ull << 3); // Y

    // Shoulders
    if (KEY_DOWN(VK_CONTROL) || KEY_DOWN(VK_LCONTROL) || KEY_DOWN('U')) *buttons |= (1ull << 6); // L
    if (KEY_DOWN('V') || KEY_DOWN('O')) *buttons |= (1ull << 7); // R
    if (KEY_DOWN(VK_SHIFT) || KEY_DOWN(VK_LSHIFT) || KEY_DOWN(VK_RSHIFT)) *buttons |= (1ull << 8); // ZL
    if (KEY_DOWN('C')) *buttons |= (1ull << 9); // ZR

    // System
    if (KEY_DOWN(VK_RETURN) || KEY_DOWN(VK_ESCAPE)) *buttons |= (1ull << 10); // Plus
    if (KEY_DOWN(VK_BACK) || KEY_DOWN(VK_TAB)) *buttons |= (1ull << 11); // Minus

    #undef KEY_DOWN
}


#endif

static void add_stick_direction_buttons(uint64_t *buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    if (lx < -16384) *buttons |= (1ull << 16);
    if (ly >  16384) *buttons |= (1ull << 17);
    if (lx >  16384) *buttons |= (1ull << 18);
    if (ly < -16384) *buttons |= (1ull << 19);

    if (rx < -16384) *buttons |= (1ull << 20);
    if (ry >  16384) *buttons |= (1ull << 21);
    if (rx >  16384) *buttons |= (1ull << 22);
    if (ry < -16384) *buttons |= (1ull << 23);
}

void hos_hid_tick(void) {
#ifdef _WIN32
    AcquireSRWLockExclusive(&g_hid_lock);
#endif
    if (!g_npad_active || !g_memory) {
#ifdef _WIN32
        ReleaseSRWLockExclusive(&g_hid_lock);
#endif
        return;
    }
    uint8_t *memory = hos_shared_memory_data(g_memory, NULL);
    if (!memory) {
#ifdef _WIN32
        ReleaseSRWLockExclusive(&g_hid_lock);
#endif
        return;
    }

    DkrGamepadState players[2] = {{0}};
    if (g_test_input_override) {
        memcpy(players, g_test_players, sizeof players);
    } else {
#ifdef _WIN32
        dkr_gamepad_poll_players(players);
        if (!game_has_focus() && !dkr_settings()->input_background)
            for (unsigned p = 0; p < 2; p++) {
                int connected = players[p].connected;
                memset(&players[p], 0, sizeof players[p]);
                players[p].connected = connected;
            }
        poll_keyboard(&players[0].buttons, &players[0].lx, &players[0].ly, &players[0].rx, &players[0].ry);
        players[0].buttons |= script_buttons();
#endif
        players[0].connected = 1; /* keyboard is always available to player 1 */
    }
    uint64_t sample = ++g_sampling_number;
    for (unsigned p = 0; p < 2; p++) {
        DkrGamepadState *pad = &players[p];
        uint8_t *slot = memory + 0x9a00 + p * 0x5000;
        uint32_t old_style, style = pad->connected && (g_supported_ids & (1u << p)) &&
            (!g_supported_styles || (g_supported_styles & 1)) ? 1 : 0;
        memcpy(&old_style, slot, 4);
        memcpy(slot, &style, 4);
        if (!style) memset(pad, 0, sizeof *pad);
        add_stick_direction_buttons(&pad->buttons, pad->lx, pad->ly, pad->rx, pad->ry);
        npad_lifo_write((NpadCommonLifo *)(slot + 0x28), sample, pad->buttons,
                        pad->lx, pad->ly, pad->rx, pad->ry, style ? 1 : 0);
        if (style) sixaxis_lifo_write((SixAxisSensorLifo *)(slot + 0x1758), sample);
        if (style != old_style && g_style_events[p]) hos_event_set(g_style_events[p], 1);
    }
    /* Compatibility alias of player 1 for clients requesting the Handheld style. */
    uint8_t *slot8 = memory + 0x9a00 + 8 * 0x5000;
    uint32_t style8 = (g_supported_ids & (1u << 8)) && (!g_supported_styles || (g_supported_styles & 2)) ? 2 : 0;
    memcpy(slot8, &style8, 4);
    DkrGamepadState *p1 = &players[0];
    npad_lifo_write((NpadCommonLifo *)(slot8 + 0x28 + 0x350), sample,
        style8 ? p1->buttons : 0, style8 ? p1->lx : 0, style8 ? p1->ly : 0,
        style8 ? p1->rx : 0, style8 ? p1->ry : 0, style8 ? 1 : 0);
    if (style8) sixaxis_lifo_write((SixAxisSensorLifo *)(slot8 + 0x1e60), sample);
#ifdef _WIN32
    ReleaseSRWLockExclusive(&g_hid_lock);
#endif
}

#ifdef _WIN32
static HANDLE g_npad_thread = NULL;
static volatile int g_npad_thread_running = 0;

typedef UINT (WINAPI *TimeBeginPeriodFn)(UINT uPeriod);
typedef UINT (WINAPI *TimeEndPeriodFn)(UINT uPeriod);

static DWORD WINAPI npad_worker_thread(LPVOID arg) {
    (void)arg;
    HMODULE winmm = LoadLibraryA("winmm.dll");
    TimeBeginPeriodFn pTimeBeginPeriod = winmm ? (TimeBeginPeriodFn)GetProcAddress(winmm, "timeBeginPeriod") : NULL;
    TimeEndPeriodFn pTimeEndPeriod = winmm ? (TimeEndPeriodFn)GetProcAddress(winmm, "timeEndPeriod") : NULL;

    if (pTimeBeginPeriod) pTimeBeginPeriod(1);

    while (g_npad_thread_running) {
        if (g_npad_active && g_memory) {
            hos_hid_tick();
        }
        Sleep(5); // ~200 Hz
    }

    if (pTimeEndPeriod) pTimeEndPeriod(1);
    if (winmm) FreeLibrary(winmm);
    return 0;
}

static void stop_npad_thread(void) {
    if (!g_npad_thread) return;
    g_npad_thread_running = 0;
    WaitForSingleObject(g_npad_thread, INFINITE);
    CloseHandle(g_npad_thread);
    g_npad_thread = NULL;
}

static void start_npad_thread(void) {
    if (g_npad_thread) return;
    static int registered_atexit = 0;
    if (!registered_atexit) {
        atexit(stop_npad_thread);
        registered_atexit = 1;
    }
    g_npad_thread_running = 1;
    g_npad_thread = CreateThread(NULL, 0, npad_worker_thread, NULL, 0, NULL);
}
#endif

static void initialize_npad(void) {
    uint8_t *memory = hos_shared_memory_data(g_memory, NULL);
    if (!memory) return;
    /* Firmware 4.x: ten 0x5000-byte slots at 0x9a00.
     * Each slot contains 7 common-state LIFOs at 0x28 (0x350 bytes each)
     * and 6 SixAxis-sensor LIFOs at 0x1758 (0x708 bytes each):
     *   0x1758: FullKey
     *   0x1e60: Handheld
     *   0x2568: JoyDual Left
     *   0x2c70: JoyDual Right
     *   0x3378: JoyLeft
     *   0x3a80: JoyRight
     */
    static const uint32_t sixaxis_offsets[6] = {0x1758, 0x1e60, 0x2568, 0x2c70, 0x3378, 0x3a80};
    for (int i = 0; i < 10; i++) {
        uint8_t *slot = memory + 0x9a00 + i * 0x5000;
        for (int ring = 0; ring < 7; ring++) {
            NpadCommonLifo *lifo = (NpadCommonLifo *)(slot + 0x28 + ring * 0x350);
            memset(lifo, 0, sizeof *lifo);
            lifo->buffer_count = 17;
        }
        for (unsigned s = 0; s < 6; s++) {
            SixAxisSensorLifo *slifo = (SixAxisSensorLifo *)(slot + sixaxis_offsets[s]);
            memset(slifo, 0, sizeof *slifo);
            slifo->buffer_count = 17;
        }
    }
}

static void applet_resource_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 0: { /* GetSharedMemoryHandle -> copy handle; the service retains its own writable view. */
        uint32_t handle = hos_copy_shared_memory_handle(g_memory);
        if (!handle) { rp->result = HOS_KERNEL(105); return; }
        rp->copy[rp->ncopy++] = handle;
        return;
    }
    default: hos_unimplemented(c, s, rq);
    }
}

void hos_hid_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    static Service applet = {"hid:IAppletResource", applet_resource_handle, NULL};
    switch (rq->cmd) {
    case 0: /* CreateAppletResource(AppletResourceUserId, PID) -> IAppletResource */
        if (rq->args_size < 8 || !rq->has_pid) { rp->result = HOS_KERNEL(103); return; }
        if (!g_memory) g_memory = hos_new_shared_memory(0x40000, 1);
        if (!g_memory) hos_stop(c, "hid: cannot create shared memory (Windows error %lu)", rt_last_os_error());
        hos_reply_interface(rp, &applet);
        return;
    case 60: case 61: case 63: case 64: case 66: case 67:
        /* SixAxis activate/deactivate/start/stop: no output */
        hos_log("hid: SixAxis command %u\n", rq->cmd);
        return;
    case 62: case 65: { /* GetSixAxisSensorLifoHandle / GetJoySixAxisSensorLifoHandle */
        if (!g_memory) { rp->result = HOS_KERNEL(106); return; }
        uint32_t handle = hos_copy_shared_memory_handle(g_memory);
        if (!handle) { rp->result = HOS_KERNEL(105); return; }
        rp->copy[rp->ncopy++] = handle;
        return;
    }
    case 68: /* IsSixAxisSensorFusionEnabled -> bool */
        RP_PUT(rp, uint8_t, 1);
        return;
    case 69: case 70: case 72:
        /* SixAxis fusion configure/reset: no output */
        return;
    case 71: /* GetSixAxisSensorFusionParameters -> float[2] */
        RP_PUT(rp, float, 0.0f);
        RP_PUT(rp, float, 0.0f);
        return;
    case 131: /* IsUnintendedHomeButtonInputProtectionEnabled -> bool */
        RP_PUT(rp, uint8_t, 0);
        return;
    case 132: /* EnableUnintendedHomeButtonInputProtection */
        return;
    case 103: /* ActivateNpad(AppletResourceUserId, PID); pre-5.0.0 protocol */
        if (rq->args_size < 8 || !rq->has_pid) { rp->result = HOS_KERNEL(103); return; }
        if (!g_memory) { rp->result = HOS_KERNEL(106); return; }
        if (!g_npad_active) initialize_npad();
        g_npad_active = 1;
#ifdef _WIN32
        start_npad_thread();
#endif
        hos_hid_tick();
        hos_log("hid: Npad activated, host input attached\n");
        return;
    case 104: /* DeactivateNpad */
        g_npad_active = 0;
        return;
    case 107: /* DisconnectNpad(u32 id, pad, u64 aruid): the host pad stays attached (Ryujinx also keeps it) */
        if (rq->args_size < 4) { rp->result = HOS_KERNEL(103); return; }
        hos_log("hid: DisconnectNpad %#x ignored; host input stays connected\n", RQ_GET(rq, uint32_t, 0));
        return;
    case 108: { /* GetPlayerLedPattern(u32 id) -> u64 pattern */
        if (rq->args_size < 4) { rp->result = HOS_KERNEL(103); return; }
        static const uint64_t patterns[8] = {0x1, 0x3, 0x7, 0xf, 0x9, 0x5, 0xd, 0x6};
        uint32_t id = RQ_GET(rq, uint32_t, 0);
        RP_PUT(rp, uint64_t, id < 8 ? patterns[id] : 0);
        return;
    }
    case 100: /* SetSupportedNpadStyleSet(u32 styles, pad, u64 aruid) */
        if (rq->args_size < 16 || !rq->has_pid) { rp->result = HOS_KERNEL(103); return; }
        g_supported_styles = RQ_GET(rq, uint32_t, 0);
        hos_log("hid: supported Npad styles %#x\n", g_supported_styles);
        return;
    case 101: /* GetSupportedNpadStyleSet */
        RP_PUT(rp, uint32_t, g_supported_styles);
        return;
    case 102: { /* SetSupportedNpadIdType(aruid, X buffer of u32 ids) */
        if (rq->nx != 1 || rq->x[0].size > 40 || (rq->x[0].size & 3) || !rq->has_pid) {
            rp->result = HOS_KERNEL(103); return;
        }
        const uint8_t *ids = (const uint8_t *)(uintptr_t)rq->x[0].addr;
        uint32_t mask = 0;
        for (uint64_t off = 0; off < rq->x[0].size; off += 4) {
            uint32_t id;
            memcpy(&id, ids + off, 4);
            int index = npad_index(id);
            if (index < 0) { rp->result = HOS_KERNEL(103); return; }
            mask |= 1u << index;
        }
        g_supported_ids = mask;
        hos_log("hid: supported Npad slots %#x\n", g_supported_ids);
        return;
    }
    case 120: /* SetNpadJoyHoldType(u64 aruid, u64 orientation) */
        if (rq->args_size < 16 || RQ_GET(rq, uint64_t, 8) > 1) { rp->result = HOS_KERNEL(103); return; }
        g_joy_hold_type = RQ_GET(rq, uint64_t, 8);
        return;
    case 121: /* GetNpadJoyHoldType */
        RP_PUT(rp, uint64_t, g_joy_hold_type);
        return;
    case 128: /* SetNpadHandheldActivationMode(u64 aruid, u64 mode) */
        if (rq->args_size < 16 || RQ_GET(rq, uint64_t, 8) > 2) { rp->result = HOS_KERNEL(103); return; }
        g_handheld_mode = RQ_GET(rq, uint64_t, 8);
        return;
    case 129: /* GetNpadHandheldActivationMode */
        RP_PUT(rp, uint64_t, g_handheld_mode);
        return;
    case 122: case 123: case 124: { /* SetNpadJoyAssignmentModeSingle(ByDefault) / Dual */
        int index = rq->args_size >= 16 ? npad_index(RQ_GET(rq, uint32_t, 0)) : -1;
        if (index < 0 || !g_memory) { rp->result = HOS_KERNEL(103); return; }
        if (rq->cmd == 123 && (rq->args_size < 24 || RQ_GET(rq, uint64_t, 16) > 1)) {
            rp->result = HOS_KERNEL(103); return;
        }
        uint8_t *memory = hos_shared_memory_data(g_memory, NULL);
        uint32_t mode = rq->cmd == 124 ? 0 : 1;
        memcpy(memory + 0x9a00 + index * 0x5000 + 4, &mode, sizeof mode);
        return;
    }
    case 106: { /* AcquireNpadStyleSetUpdateEventHandle(u32 id, pad, u64 aruid, u64 event pointer) */
        int index = rq->args_size >= 24 ? npad_index(RQ_GET(rq, uint32_t, 0)) : -1;
        if (index < 0 || !rq->has_pid) { rp->result = HOS_KERNEL(103); return; }
        if (!g_style_events[index]) g_style_events[index] = hos_new_event(1);
        uint32_t copy = hos_copy_event_handle(g_style_events[index]);
        if (!copy) { rp->result = HOS_KERNEL(105); return; }
        rp->copy[rp->ncopy++] = copy;
        return;
    }
    case 203: { /* CreateActiveVibrationDeviceList -> IActiveVibrationDeviceList */
        Service *list = calloc(1, sizeof *list);
        VibrationList *state = calloc(1, sizeof *state);
        if (!list || !state) { free(list); free(state); rp->result = HOS_KERNEL(104); return; }
        *list = (Service){"hid:IActiveVibrationDeviceList", vibration_list_handle, state, vibration_list_destroy};
        hos_reply_interface(rp, list);
        return;
    }
    case 201: /* SendVibrationValue(u32 handle at +0, VibrationValue at +4, pad, u64 aruid at +0x18): the SDK
               * packs the 4-byte-aligned VibrationValue right after the handle (nn::hid SendVibrationValue). */
        if (rq->args_size < 20) { rp->result = HOS_KERNEL(103); return; }
        vibration_send(RQ_GET(rq, uint32_t, 0), rq->args + 4);
        return;
    case 202: { /* GetActualVibrationValue(u32 handle, pad, u64 aruid) -> VibrationValue of that motor */
        if (rq->args_size < 4) { rp->result = HOS_KERNEL(103); return; }
        const VibrationDevice *d = vibration_device(RQ_GET(rq, uint32_t, 0), 0);
        memcpy(rp->data + rp->data_size, d ? d->value : g_vibration_neutral, sizeof g_vibration_neutral);
        rp->data_size += sizeof g_vibration_neutral;
        return;
    }
    case 206: { /* SendVibrationValues(u64 aruid, handles[], values[]): the SDK passes two X (pointer) buffers */
        const IpcBuf *buffers = rq->nx >= 2 ? rq->x : rq->na >= 2 ? rq->a : NULL;
        if (!buffers) return;
        uint64_t count = buffers[0].size / 4, values = buffers[1].size / 16;
        if (values < count) count = values;
        const uint8_t *handles = (const uint8_t *)(uintptr_t)buffers[0].addr, *data = (const uint8_t *)(uintptr_t)buffers[1].addr;
        for (uint64_t i = 0; i < count && handles && data; i++) {
            uint32_t handle; memcpy(&handle, handles + 4 * i, 4);
            vibration_send(handle, data + 16 * i);
        }
        return;
    }
    case 204: /* PermitVibration(bool) */
        if (!rq->args_size) { rp->result = HOS_KERNEL(103); return; }
        g_vibration_permitted = rq->args[0] != 0;
        vibration_apply(0); vibration_apply(1); /* stops both pads, or restores what the game last sent */
        return;
    case 205: /* IsVibrationPermitted -> bool */
        RP_PUT(rp, uint8_t, g_vibration_permitted);
        return;
    default: hos_unimplemented(c, s, rq);
    }
}
