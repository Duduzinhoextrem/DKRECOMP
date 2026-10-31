/* Minimal VI display service for the native graphics path. Protocol: switchbrew VI services, firmware 4.x.
 * The host exposes one logical 1280x720 display. Binder/layer operations stop until their implementation exists. */
#include "hos.h"
#include <stdlib.h>

#define VI_INVALID_ARGUMENT 0x272u
#define VI_PERMISSION_DENIED 0xa72u
#define DISPLAY_ID 1ull
#define MAX_LAYERS 64

typedef struct ViApplication { int privilege; unsigned display_opens; } ViApplication;
typedef struct ViLayer {
    uint64_t id;
    uint32_t binder_id, opens, available_event;
    int32_t weak_refs, strong_refs;
} ViLayer;
static ViLayer g_layers[MAX_LAYERS];
static uint64_t g_next_layer = 1;

uint64_t hos_vi_create_managed_layer(void) {
    for (uint32_t i = 0; i < MAX_LAYERS; i++) {
        if (g_layers[i].id) continue;
        g_layers[i] = (ViLayer){.id = g_next_layer++, .binder_id = i + 1};
        return g_layers[i].id;
    }
    return 0;
}

static ViLayer *layer_of(uint64_t id) {
    for (int i = 0; i < MAX_LAYERS; i++) if (id && g_layers[i].id == id) return &g_layers[i];
    return NULL;
}

static void binder_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    uint32_t id = rq->args_size >= 4 ? RQ_GET(rq, uint32_t, 0) : 0;
    ViLayer *layer = id && id <= MAX_LAYERS && g_layers[id - 1].id ? &g_layers[id - 1] : NULL;
    if (!layer) { rp->result = VI_INVALID_ARGUMENT; return; }
    switch (rq->cmd) {
    case 1: { /* AdjustRefcount(s32 binder id, s32 delta, s32 type: 0 weak, 1 strong) */
        if (rq->args_size < 12 || RQ_GET(rq, uint32_t, 8) > 1) { rp->result = VI_INVALID_ARGUMENT; return; }
        int32_t *refs = RQ_GET(rq, uint32_t, 8) ? &layer->strong_refs : &layer->weak_refs;
        int64_t total = (int64_t)*refs + RQ_GET(rq, int32_t, 4);
        if (total < 0 || total > INT32_MAX) { rp->result = VI_INVALID_ARGUMENT; return; }
        *refs = (int32_t)total;
        hos_log("vi: Binder %u refs weak=%d strong=%d\n", id, layer->weak_refs, layer->strong_refs);
        return;
    }
    case 2: { /* GetNativeHandle(binder id, 0xf) -> buffer-availability event (not display VSync). */
        if (rq->args_size < 8) { rp->result = VI_INVALID_ARGUMENT; return; }
        if (RQ_GET(rq, uint32_t, 4) != 0xf)
            hos_stop(c, "vi: Binder native handle type %#x is not implemented", RQ_GET(rq, uint32_t, 4));
        /* No producer has dequeued a buffer from this layer, so slots are initially available. Queue operations
         * remain explicit stops until implemented; this event does not report a frame or GPU completion. */
        if (!layer->available_event) layer->available_event = hos_new_event(1);
        uint32_t copy = hos_copy_event_handle(layer->available_event);
        if (!copy) { rp->result = HOS_KERNEL(105); return; }
        rp->copy[rp->ncopy++] = copy;
        return;
    }
    default: hos_unimplemented(c, s, rq);
    }
}

static void put_u32(uint8_t *dst, uint32_t value) { memcpy(dst, &value, 4); }

/* Public NativeWindow wire format: a 0x28-byte flattened Binder and one object-offset entry.
 * NativeWindow is parsed by the original SDK; it is not a pointer to a guessed SDK object layout. */
static void write_native_window(uint8_t *out, const ViLayer *layer) {
    memset(out, 0, 0x3c);
    put_u32(out, 0x28); put_u32(out + 4, 0x10);
    put_u32(out + 8, 4); put_u32(out + 12, 0x38);
    put_u32(out + 0x10, 2); /* remote Binder */
    put_u32(out + 0x18, layer->binder_id);
    memcpy(out + 0x28, "dispdrv", 8);
}

static void vi_destroy(Service *s) { free(s); }
static void vi_application_destroy(Service *s) { free(s->state); free(s); }

static Service *vi_service(Cpu *c, const char *name, ServiceFn fn, int privilege) {
    Service *s = calloc(1, sizeof *s);
    if (!s) hos_stop(c, "vi: cannot allocate service %s", name);
    strncpy(s->name, name, sizeof s->name - 1);
    s->handle = fn;
    s->state = (void *)(uintptr_t)privilege;
    s->destroy = vi_destroy;
    return s;
}

static void vi_pending(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    (void)rp;
    hos_unimplemented(c, s, rq);
}

static void vi_application(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    ViApplication *app = s->state;
    int privilege = app->privilege;
    switch (rq->cmd) {
    case 100: /* GetRelayService */
        hos_reply_interface(rp, vi_service(c, "vi:IHOSBinderDriver", binder_handle, privilege));
        return;
    case 101: /* GetSystemDisplayService */
        if (privilege < 1) { rp->result = VI_PERMISSION_DENIED; return; }
        hos_reply_interface(rp, vi_service(c, "vi:ISystemDisplayService", vi_pending, privilege));
        return;
    case 102: /* GetManagerDisplayService */
        if (privilege < 2) { rp->result = VI_PERMISSION_DENIED; return; }
        hos_reply_interface(rp, vi_service(c, "vi:IManagerDisplayService", vi_pending, privilege));
        return;
    case 103: /* GetIndirectDisplayTransactionService */
        if (privilege < 1) { rp->result = VI_PERMISSION_DENIED; return; }
        hos_reply_interface(rp, vi_service(c, "vi:IIndirectBinderDriver", vi_pending, privilege));
        return;
    case 1010: /* OpenDisplay(DisplayName[0x40]) */
        if (rq->args_size < 0x40 || memcmp(rq->args, "Default", 8)) { rp->result = VI_INVALID_ARGUMENT; return; }
        /* fall through */
    case 1011: /* OpenDefaultDisplay */
        app->display_opens++;
        hos_log("vi: opened logical display Default (%llu), 1280x720\n", DISPLAY_ID);
        RP_PUT(rp, uint64_t, DISPLAY_ID);
        return;
    case 1020: /* CloseDisplay */
        if (rq->args_size < 8 || RQ_GET(rq, uint64_t, 0) != DISPLAY_ID || !app->display_opens) {
            rp->result = VI_INVALID_ARGUMENT; return;
        }
        app->display_opens--;
        return;
    case 1102: /* GetDisplayResolution(DisplayId) -> s64 width, s64 height */
        if (rq->args_size < 8 || RQ_GET(rq, uint64_t, 0) != DISPLAY_ID) {
            rp->result = VI_INVALID_ARGUMENT; return;
        }
        RP_PUT(rp, int64_t, 1280);
        RP_PUT(rp, int64_t, 720);
        return;
    case 2020: { /* OpenLayer(DisplayName, LayerId, AppletResourceUserId; PID; B NativeWindow) */
        if (rq->args_size < 80 || !rq->has_pid || memcmp(rq->args, "Default", 8) ||
            RQ_GET(rq, uint64_t, 72) != 0x10 || rq->nb != 1 || rq->b[0].size < 0x3c) {
            rp->result = VI_INVALID_ARGUMENT; return;
        }
        ViLayer *layer = layer_of(RQ_GET(rq, uint64_t, 64));
        if (!layer) { rp->result = VI_INVALID_ARGUMENT; return; }
        write_native_window((uint8_t *)(uintptr_t)rq->b[0].addr, layer);
        layer->opens++;
        RP_PUT(rp, uint64_t, 0x3c);
        hos_log("vi: opened layer %llu, Binder %u, NativeWindow parcel 0x3c bytes\n",
                (unsigned long long)layer->id, layer->binder_id);
        return;
    }
    case 2021: { /* CloseLayer */
        ViLayer *layer = rq->args_size >= 8 ? layer_of(RQ_GET(rq, uint64_t, 0)) : NULL;
        if (!layer || !layer->opens) { rp->result = VI_INVALID_ARGUMENT; return; }
        layer->opens--;
        return;
    }
    default: hos_unimplemented(c, s, rq);
    }
}

void hos_vi_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    int privilege = !strcmp(s->name, "vi:m") ? 2 : !strcmp(s->name, "vi:s") ? 1 : 0;
    if (rq->cmd != (uint32_t)privilege) hos_unimplemented(c, s, rq);
    if (rq->args_size < 4 || RQ_GET(rq, uint32_t, 0) > 1) { rp->result = VI_INVALID_ARGUMENT; return; }
    if (RQ_GET(rq, uint32_t, 0) == 1 && !privilege) { rp->result = VI_PERMISSION_DENIED; return; }
    if (!RQ_GET(rq, uint32_t, 0)) privilege = 0;
    ViApplication *app = calloc(1, sizeof *app);
    if (!app) { rp->result = HOS_KERNEL(104); return; }
    app->privilege = privilege;
    Service *service = vi_service(c, "vi:IApplicationDisplayService", vi_application, 0);
    service->state = app;
    service->destroy = vi_application_destroy;
    hos_reply_interface(rp, service);
}
