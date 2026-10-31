/* Initial HID service: the applet resource owns the service's 256 KiB input shared memory.
 * Command/layout reference: switchbrew HID services, firmware 4.x. Initially all controllers are disconnected.
 * Activation/configuration and host input are implemented only as their commands are reached by the guest. */
#include "hos.h"
#include <stdlib.h>

static uint32_t g_memory;
static int g_npad_active;
static uint64_t g_joy_hold_type;
static uint64_t g_handheld_mode;
static uint32_t g_supported_styles, g_supported_ids = 0x3ff;
static uint32_t g_style_events[10];
static int g_vibration_permitted = 1;

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

static void initialize_npad(void) {
    uint8_t *memory = hos_shared_memory_data(g_memory, NULL);
    if (!memory) return;
    /* Firmware 4.x: ten 0x5000-byte slots at 0x9a00, each with seven 0x350-byte common-state LIFOs.
     * No input backend has attached a controller yet. The rings are valid and empty, style_set remains zero. */
    for (int i = 0; i < 10; i++) {
        uint8_t *slot = memory + 0x9a00 + i * 0x5000;
        for (int ring = 0; ring < 7; ring++) {
            uint64_t capacity = 17;
            memcpy(slot + 0x28 + ring * 0x350 + 8, &capacity, sizeof capacity);
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
    case 103: /* ActivateNpad(AppletResourceUserId, PID); pre-5.0.0 protocol */
        if (rq->args_size < 8 || !rq->has_pid) { rp->result = HOS_KERNEL(103); return; }
        if (!g_memory) { rp->result = HOS_KERNEL(106); return; }
        if (!g_npad_active) initialize_npad();
        g_npad_active = 1;
        hos_log("hid: Npad activated, no host controllers attached\n");
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
