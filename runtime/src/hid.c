/* Initial HID service: the applet resource owns the service's 256 KiB input shared memory.
 * Command/layout reference: switchbrew HID services, firmware 4.x. Initially all controllers are disconnected.
 * Host input (keyboard + XInput) polls asynchronously and updates shared memory Npad rings at ~200 Hz. */
#include "hos.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define DKR_BARRIER() MemoryBarrier()
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
static uint64_t g_sampling_number = 0;

static int g_test_input_override = 0;
static uint64_t g_test_buttons = 0;
static int32_t g_test_lx = 0, g_test_ly = 0, g_test_rx = 0, g_test_ry = 0;

void hos_hid_set_test_input(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    g_test_input_override = 1;
    g_test_buttons = buttons;
    g_test_lx = lx;
    g_test_ly = ly;
    g_test_rx = rx;
    g_test_ry = ry;
}

void hos_hid_clear_test_input(void) {
    g_test_input_override = 0;
    g_test_buttons = 0;
    g_test_lx = 0;
    g_test_ly = 0;
    g_test_rx = 0;
    g_test_ry = 0;
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

typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XINPUT_GAMEPAD_LOCAL;

typedef struct {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD_LOCAL Gamepad;
} XINPUT_STATE_LOCAL;

typedef DWORD (WINAPI *XInputGetState_Fn)(DWORD dwUserIndex, XINPUT_STATE_LOCAL *pState);
static XInputGetState_Fn pXInputGetState = NULL;
static int g_xinput_loaded = 0;

static void load_xinput(void) {
    if (g_xinput_loaded) return;
    g_xinput_loaded = 1;
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (!h) h = LoadLibraryA("xinput1_3.dll");
    if (!h) h = LoadLibraryA("xinput9_1_0.dll");
    if (h) {
        pXInputGetState = (XInputGetState_Fn)GetProcAddress(h, "XInputGetState");
    }
}

static void poll_keyboard(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    (void)rx; (void)ry;
    if (!g_user32_loaded) load_user32();
    if (!pGetAsyncKeyState) return;

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

static void poll_xinput(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (!g_xinput_loaded) load_xinput();
    if (!pXInputGetState) return;

    XINPUT_STATE_LOCAL st;
    if (pXInputGetState(0, &st) != 0) return;

    WORD b = st.Gamepad.wButtons;
    if (b & 0x1000) *buttons |= (1ull << 1); // A -> Switch B
    if (b & 0x2000) *buttons |= (1ull << 0); // B -> Switch A
    if (b & 0x4000) *buttons |= (1ull << 3); // X -> Switch Y
    if (b & 0x8000) *buttons |= (1ull << 2); // Y -> Switch X

    if (b & 0x0001) *buttons |= (1ull << 13); // Up
    if (b & 0x0002) *buttons |= (1ull << 15); // Down
    if (b & 0x0004) *buttons |= (1ull << 12); // Left
    if (b & 0x0008) *buttons |= (1ull << 14); // Right

    if (b & 0x0100) *buttons |= (1ull << 6); // LB -> L
    if (b & 0x0200) *buttons |= (1ull << 7); // RB -> R
    if (st.Gamepad.bLeftTrigger > 30)  *buttons |= (1ull << 8); // LT -> ZL
    if (st.Gamepad.bRightTrigger > 30) *buttons |= (1ull << 9); // RT -> ZR
    if (b & 0x0040) *buttons |= (1ull << 4); // LS -> Stick L
    if (b & 0x0080) *buttons |= (1ull << 5); // RS -> Stick R
    if (b & 0x0010) *buttons |= (1ull << 10); // Start -> Plus
    if (b & 0x0020) *buttons |= (1ull << 11); // Back -> Minus

    // Left stick with deadzone (~4000)
    int sx = st.Gamepad.sThumbLX, sy = st.Gamepad.sThumbLY;
    if (abs(sx) > 4000) {
        if (sx > 32767) sx = 32767; else if (sx < -32767) sx = -32767;
        *lx = sx;
    }
    if (abs(sy) > 4000) {
        if (sy > 32767) sy = 32767; else if (sy < -32767) sy = -32767;
        *ly = sy;
    }

    // Right stick with deadzone (~4000)
    int rsx = st.Gamepad.sThumbRX, rsy = st.Gamepad.sThumbRY;
    if (abs(rsx) > 4000) {
        if (rsx > 32767) rsx = 32767; else if (rsx < -32767) rsx = -32767;
        *rx = rsx;
    }
    if (abs(rsy) > 4000) {
        if (rsy > 32767) rsy = 32767; else if (rsy < -32767) rsy = -32767;
        *ry = rsy;
    }
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
    if (!g_npad_active || !g_memory) return;
    uint8_t *memory = hos_shared_memory_data(g_memory, NULL);
    if (!memory) return;

    uint64_t buttons = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;

    if (g_test_input_override) {
        buttons = g_test_buttons;
        lx = g_test_lx;
        ly = g_test_ly;
        rx = g_test_rx;
        ry = g_test_ry;
    } else {
#ifdef _WIN32
        poll_keyboard(&buttons, &lx, &ly, &rx, &ry);
        poll_xinput(&buttons, &lx, &ly, &rx, &ry);
#endif
    }
    add_stick_direction_buttons(&buttons, lx, ly, rx, ry);

    uint64_t sample = ++g_sampling_number;

    // Slot 0 (Player 1)
    uint8_t *slot0 = memory + 0x9a00;
    uint32_t style0 = (1u << 0); // FullKey (1) - single active controller style
    if (g_supported_styles && (g_supported_styles & style0)) {
        style0 = g_supported_styles & style0;
    }
    memcpy(slot0 + 0, &style0, sizeof style0);
    npad_lifo_write((NpadCommonLifo *)(slot0 + 0x28), sample, buttons, lx, ly, rx, ry, 1);
    sixaxis_lifo_write((SixAxisSensorLifo *)(slot0 + 0x1758), sample);

    // Slot 8 (Handheld)
    uint8_t *slot8 = memory + 0x9a00 + 8 * 0x5000;
    uint32_t style8 = (1u << 1); // Handheld (2)
    memcpy(slot8 + 0, &style8, sizeof style8);
    npad_lifo_write((NpadCommonLifo *)(slot8 + 0x28 + 1 * 0x350), sample, buttons, lx, ly, rx, ry, 1);
    sixaxis_lifo_write((SixAxisSensorLifo *)(slot8 + 0x1e60), sample);
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
    WaitForSingleObject(g_npad_thread, 200);
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
    case 204: /* PermitVibration(bool) */
        if (!rq->args_size) { rp->result = HOS_KERNEL(103); return; }
        g_vibration_permitted = rq->args[0] != 0;
        return;
    case 205: /* IsVibrationPermitted -> bool */
        RP_PUT(rp, uint8_t, g_vibration_permitted);
        return;
    default: hos_unimplemented(c, s, rq);
    }
}
