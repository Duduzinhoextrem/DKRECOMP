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
    uint64_t id, scaling_mode;
    uint32_t binder_id, opens, available_event;
    int32_t weak_refs, strong_refs;
    uint32_t connected_api, producer_controlled;
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

typedef struct ParcelReader { const uint8_t *data; uint32_t size, position; } ParcelReader;
static uint32_t read_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void put_u32(uint8_t *dst, uint32_t value) { memcpy(dst, &value, 4); }

static const IpcBuf *single_buffer(const IpcBuf *a, int na, const IpcBuf *b, int nb) {
    const IpcBuf *found = NULL;
    for (int set = 0; set < 2; set++) {
        const IpcBuf *buffers = set ? b : a;
        int count = set ? nb : na;
        for (int i = 0; i < count; i++) {
            if (!buffers[i].size) continue;
            if (found || !buffers[i].addr) return NULL;
            found = &buffers[i];
        }
    }
    return found;
}

static int open_parcel(const IpcBuf *buffer, ParcelReader *reader) {
    if (!buffer || buffer->size < 16) return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)buffer->addr;
    uint32_t size = read_u32(p), offset = read_u32(p + 4);
    uint32_t objects_size = read_u32(p + 8), objects_offset = read_u32(p + 12);
    if (offset < 16 || (offset & 3) || (size & 3) || offset > buffer->size || size > buffer->size - offset) return 0;
    if (objects_size && (objects_offset < 16 || (objects_offset & 3) || (objects_size & 3) ||
        objects_offset > buffer->size || objects_size > buffer->size - objects_offset)) return 0;
    *reader = (ParcelReader){p + offset, size, 0};
    return 1;
}

static int parcel_u32(ParcelReader *reader, uint32_t *value) {
    if (reader->position > reader->size || reader->size - reader->position < 4) return 0;
    *value = read_u32(reader->data + reader->position);
    reader->position += 4;
    return 1;
}

static int producer_token(ParcelReader *reader) {
    static const char token[] = "android.gui.IGraphicBufferProducer";
    uint32_t policy, length;
    if (!parcel_u32(reader, &policy) || !parcel_u32(reader, &length) || length != sizeof token - 1) return 0;
    uint32_t bytes = (uint32_t)((sizeof token * 2 + 3) & ~3u);
    if (reader->size - reader->position < bytes) return 0;
    const uint8_t *str = reader->data + reader->position;
    for (unsigned i = 0; i < sizeof token; i++)
        if (str[i * 2] != (uint8_t)token[i] || str[i * 2 + 1] != 0) return 0;
    reader->position += bytes;
    (void)policy; /* Android StrictMode policy is transported but does not affect this native host service. */
    return 1;
}

static void write_parcel(const IpcBuf *buffer, const uint32_t *words, unsigned count) {
    uint8_t *p = (uint8_t *)(uintptr_t)buffer->addr;
    put_u32(p, count * 4); put_u32(p + 4, 16);
    put_u32(p + 8, 0); put_u32(p + 12, 16 + count * 4);
    memcpy(p + 16, words, count * 4);
}

static void transact_parcel(Cpu *c, Service *s, ViLayer *layer, const IpcRequest *rq, IpcReply *rp) {
    if (rq->args_size < 12) { rp->result = VI_INVALID_ARGUMENT; return; }
    uint32_t code = RQ_GET(rq, uint32_t, 4);
    const IpcBuf *input = single_buffer(rq->x, rq->nx, rq->a, rq->na);
    const IpcBuf *output = single_buffer(rq->c, rq->nc, rq->b, rq->nb);
    ParcelReader reader;
    if (!output || !open_parcel(input, &reader) || !producer_token(&reader)) {
        rp->result = VI_INVALID_ARGUMENT; return;
    }
    switch (code) {
    case 10: { /* IGraphicBufferProducer::connect(NULL listener, API, producerControlledByApp) */
        uint32_t listener, api, controlled;
        if (!parcel_u32(&reader, &listener) || !parcel_u32(&reader, &api) || !parcel_u32(&reader, &controlled) ||
            output->size < 0x24 || !api || api > 4 || controlled > 1) { rp->result = VI_INVALID_ARGUMENT; return; }
        if (listener) hos_stop(c, "vi: Binder Connect with a listener is not implemented");
        int32_t status = layer->connected_api ? -22 : 0; /* Android BAD_VALUE on a second connection */
        if (!status) { layer->connected_api = api; layer->producer_controlled = controlled; }
        uint32_t reply[] = {1280, 720, 0, 0, (uint32_t)status}; /* BqBufferOutput followed by status_t */
        write_parcel(output, reply, sizeof reply / sizeof *reply);
        hos_log("vi: Binder %u Connect API=%u app-controlled=%u status=%d\n", layer->binder_id, api, controlled, status);
        return;
    }
    case 11: { /* IGraphicBufferProducer::disconnect(API) */
        uint32_t api;
        if (!parcel_u32(&reader, &api) || output->size < 20) { rp->result = VI_INVALID_ARGUMENT; return; }
        uint32_t status = (uint32_t)-22;
        if (api && layer->connected_api == api) {
            layer->connected_api = 0; layer->producer_controlled = 0; status = 0;
        }
        write_parcel(output, &status, 1);
        return;
    }
    default:
        hos_dump_request(rq);
        hos_stop(c, "%s transaction %u is not implemented", s->name, code);
    }
}

static void binder_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    uint32_t id = rq->args_size >= 4 ? RQ_GET(rq, uint32_t, 0) : 0;
    ViLayer *layer = id && id <= MAX_LAYERS && g_layers[id - 1].id ? &g_layers[id - 1] : NULL;
    if (!layer) { rp->result = VI_INVALID_ARGUMENT; return; }
    switch (rq->cmd) {
    case 0: case 3: /* TransactParcel / TransactParcelAuto */
        transact_parcel(c, s, layer, rq, rp);
        return;
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
    case 2101: { /* SetLayerScalingMode(u64 mode, u64 layer id): retained for the presentation adapter. */
        ViLayer *layer = rq->args_size >= 16 ? layer_of(RQ_GET(rq, uint64_t, 8)) : NULL;
        if (!layer || !layer->opens) { rp->result = VI_INVALID_ARGUMENT; return; }
        layer->scaling_mode = RQ_GET(rq, uint64_t, 0);
        hos_log("vi: layer %llu scaling mode %llu\n", (unsigned long long)layer->id,
                (unsigned long long)layer->scaling_mode);
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
