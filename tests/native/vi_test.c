/* Display service hierarchy, permissions, logical mode and handle lifetime through real CMIF requests. */
#include "../../runtime/src/hos.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
void hos_log(const char *fmt, ...) { (void)fmt; }
void hos_stop(Cpu *c, const char *fmt, ...) {
    (void)c; va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); exit(2);
}
void kern_sync_poke(uint32_t handle) { (void)handle; }
Service *hos_get_service(const char *name) { (void)name; return NULL; }

static uint8_t msg[0x100];
static Cpu cpu;
static void put32(int off, uint32_t value) { memcpy(msg + off, &value, 4); }
static uint32_t get32(int off) { uint32_t value; memcpy(&value, msg + off, 4); return value; }
static uint64_t get64(int off) { uint64_t value; memcpy(&value, msg + off, 8); return value; }
static const uint64_t native_buffer = 0x3900100000ull;

static uint32_t open_layer(uint32_t app, uint64_t layer, uint32_t capacity) {
    memset(msg, 0, sizeof msg);
    put32(0, 0x01000004); put32(4, 0x80000018); put32(8, 1); /* PID, one B descriptor */
    put32(20, capacity); put32(24, (uint32_t)native_buffer);
    put32(28, ((uint32_t)(native_buffer >> 32) & 15) << 28 | ((uint32_t)(native_buffer >> 36) & 7) << 2);
    put32(0x20, 0x49434653); put32(0x28, 2020);
    memcpy(msg + 0x30, "Default", 8); memcpy(msg + 0x70, &layer, 8);
    uint64_t aruid = 0x10;
    memcpy(msg + 0x78, &aruid, 8);
    CHECK(hos_send_sync_request(&cpu, app, msg, sizeof msg) == HOS_OK);
    return get32(0x18);
}

static uint32_t send(uint32_t handle, uint32_t cmd, const void *args, uint32_t size) {
    memset(msg, 0, sizeof msg);
    put32(0, 4); put32(4, 6 + (size + 3) / 4);
    put32(0x10, 0x49434653); put32(0x18, cmd);
    if (size) memcpy(msg + 0x20, args, size);
    CHECK(hos_send_sync_request(&cpu, handle, msg, sizeof msg) == HOS_OK);
    return get32(0x18);
}

static uint32_t open_app(Service *root, uint32_t command, uint32_t mode) {
    uint32_t session = hos_new_session(root);
    CHECK(send(session, command, &mode, 4) == HOS_OK);
    uint32_t app = get32(12);
    CHECK(app && (get32(8) == 0x20));
    hos_close_handle(session);
    return app;
}

int main(void) {
    Service manager = {"vi:m", hos_vi_handle, NULL};
    Service user = {"vi:u", hos_vi_handle, NULL};
    Service system = {"vi:s", hos_vi_handle, NULL};
    uint32_t root = hos_new_session(&user), mode = 1;
    CHECK(send(root, 0, &mode, 4) == 0xa72); /* user root cannot elevate itself */
    hos_close_handle(root);
    uint32_t app = open_app(&manager, 2, 1);
    uint32_t regular = open_app(&manager, 2, 0);
    uint32_t sys = open_app(&system, 1, 1);
    CHECK(send(regular, 101, NULL, 0) == 0xa72 && send(regular, 102, NULL, 0) == 0xa72);
    CHECK(send(sys, 102, NULL, 0) == 0xa72);
    CHECK(send(sys, 101, NULL, 0) == HOS_OK);
    hos_close_handle(get32(12));
    const uint32_t interfaces[] = {100, 101, 102, 103};
    uint32_t binder = 0;
    for (unsigned i = 0; i < sizeof interfaces / sizeof *interfaces; i++) {
        CHECK(send(app, interfaces[i], NULL, 0) == HOS_OK);
        uint32_t child = get32(12);
        CHECK(child);
        if (interfaces[i] == 100) binder = child;
        else hos_close_handle(child);
    }
    char name[0x40] = "invalid";
    CHECK(send(app, 1010, name, sizeof name) != HOS_OK);
    memcpy(name, "Default", 8);
    CHECK(send(app, 1010, name, sizeof name) == HOS_OK);
    uint64_t display = get64(0x20);
    CHECK(display == 1);
    CHECK(send(app, 1102, &display, 8) == HOS_OK && get64(0x20) == 1280 && get64(0x28) == 720);
    uint64_t invalid = 999;
    CHECK(send(app, 1102, &invalid, 8) != HOS_OK);
    CHECK(send(app, 1011, NULL, 0) == HOS_OK && get64(0x20) == display);
    CHECK(send(app, 1020, &display, 8) == HOS_OK);
    CHECK(send(app, 1020, &display, 8) == HOS_OK);
    CHECK(send(app, 1020, &display, 8) != HOS_OK);
    CHECK(rt_map_fixed(native_buffer, 0x1000) == 0);
    uint8_t *parcel = (uint8_t *)(uintptr_t)native_buffer;
    memset(parcel, 0xee, 0x1000);
    uint64_t layer = hos_vi_create_managed_layer();
    CHECK(layer && hos_vi_create_managed_layer() != layer);
    CHECK(open_layer(app, layer, 0x3b) != HOS_OK && parcel[0] == 0xee);
    CHECK(open_layer(app, 999, 0x100) != HOS_OK && parcel[0] == 0xee);
    CHECK(open_layer(app, layer, 0x100) == HOS_OK && get64(0x20) == 0x3c);
    uint32_t fields[4];
    memcpy(fields, parcel, sizeof fields);
    CHECK(fields[0] == 0x28 && fields[1] == 0x10 && fields[2] == 4 && fields[3] == 0x38);
    CHECK(!memcmp(parcel + 0x28, "dispdrv", 8) && parcel[0x3c] == 0xee);
    uint32_t binder_id;
    memcpy(&binder_id, parcel + 0x18, 4);
    CHECK(binder_id);
    int32_t refs[3] = {(int32_t)binder_id, -1, 0};
    CHECK(send(binder, 1, refs, sizeof refs) != HOS_OK); /* cannot underflow weak refs */
    refs[1] = 1;
    CHECK(send(binder, 1, refs, sizeof refs) == HOS_OK);
    refs[2] = 1;
    CHECK(send(binder, 1, refs, sizeof refs) == HOS_OK);
    refs[1] = -1;
    CHECK(send(binder, 1, refs, sizeof refs) == HOS_OK);
    CHECK(send(binder, 1, refs, sizeof refs) != HOS_OK);
    uint32_t native_args[2] = {binder_id, 0xf};
    CHECK(send(binder, 2, native_args, sizeof native_args) == HOS_OK && get32(8) == 2);
    uint32_t available = get32(12);
    CHECK(hos_event_state(available) == 1);
    hos_event_set(available, 0);
    CHECK(send(binder, 2, native_args, sizeof native_args) == HOS_OK);
    uint32_t available_copy = get32(12);
    CHECK(available_copy != available && hos_event_state(available_copy) == 0);
    hos_close_handle(available); hos_close_handle(available_copy);
    CHECK(send(app, 2021, &layer, 8) == HOS_OK && send(app, 2021, &layer, 8) != HOS_OK);
    hos_close_handle(binder);
    rt_unmap(native_buffer);
    hos_close_handle(app); hos_close_handle(regular); hos_close_handle(sys);
    printf("VI display: %s (permissions, interfaces, mode, layer parcel, bounds, Binder refs/events, close)\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
