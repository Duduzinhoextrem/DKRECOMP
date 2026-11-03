/* Real aliasing, page alignment, permissions and object lifetime; synthetic data only. */
#include "../../runtime/src/hos.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d %s (Windows error %lu)\n", __FILE__, __LINE__, #cond, GetLastError()); failures++; \
} } while (0)

void hos_log(const char *fmt, ...) { (void)fmt; }
void hos_stop(Cpu *c, const char *fmt, ...) {
    (void)c;
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    exit(2);
}
void kern_sync_poke(uint32_t handle) { (void)handle; }
Service *hos_get_service(const char *name) { (void)name; return NULL; }

static const uint64_t first = 0x3900001000ull, second = 0x3900007000ull;

static void test_views(void) {
    uint32_t handle = hos_new_shared_memory(0x3000, 3);
    CHECK(handle != 0);
    uint64_t size = 0;
    volatile uint32_t *host = hos_shared_memory_data(handle, &size);
    CHECK(host && size == 0x3000 && host[0] == 0 && host[0xbff] == 0);
    if (!handle || !host) return;
    host[0] = 0x12345678; host[0xbff] = 0x87654321;
    CHECK(hos_map_shared_memory(handle, first + 1, size, 3) == HOS_KERNEL(102));
    CHECK(hos_map_shared_memory(handle, first, 0, 3) == HOS_KERNEL(101));
    CHECK(hos_map_shared_memory(handle, first, size - 0x1000, 3) == HOS_KERNEL(101));
    CHECK(hos_map_shared_memory(handle, first, size, 5) == HOS_KERNEL(108));
    CHECK(hos_map_shared_memory(0xdead, first, size, 3) == HOS_INVALID_HANDLE);
    uint32_t result = hos_map_shared_memory(handle, first, size, 3);
    CHECK(result == HOS_OK);
    if (result) { hos_close_shared_memory(handle); return; }
    CHECK(hos_map_shared_memory(handle, second, size, 3) == HOS_OK); /* same 64 KiB envelope */
    volatile uint32_t *a = (volatile uint32_t *)(uintptr_t)first;
    volatile uint32_t *b = (volatile uint32_t *)(uintptr_t)second;
    CHECK(a[0] == 0x12345678 && a[0xbff] == 0x87654321 && b[0] == a[0]);
    host[4] = 0x10203040;
    CHECK(a[4] == 0x10203040 && b[4] == 0x10203040);
    b[4] = 0x55667788;
    CHECK(host[4] == 0x55667788 && a[4] == 0x55667788);
    CHECK(hos_map_shared_memory(handle, first, size, 3) == HOS_KERNEL(106));
    uint32_t other = hos_new_shared_memory(size, 3);
    CHECK(other != 0 && hos_unmap_shared_memory(other, first, size) == HOS_KERNEL(106));
    CHECK(hos_unmap_shared_memory(handle, first, size - 0x1000) == HOS_KERNEL(106));
    CHECK(hos_unmap_shared_memory(handle, first, size) == HOS_OK);
    CHECK(b[4] == 0x55667788);
    CHECK(hos_map_shared_memory(other, first, size, 3) == HOS_OK);
    CHECK(a[4] == 0 && b[4] == 0x55667788); /* unmapped hole can hold a different object */
    CHECK(hos_unmap_shared_memory(other, first, size) == HOS_OK);
    CHECK(hos_close_shared_memory(other));
    uint32_t copy = hos_copy_shared_memory_handle(handle);
    CHECK(copy && copy != handle);
    CHECK(hos_close_shared_memory(handle));
    CHECK(!hos_shared_memory_data(handle, NULL));
    CHECK(b[4] == 0x55667788 && hos_shared_memory_data(copy, NULL) == (void *)host);
    CHECK(hos_unmap_shared_memory(copy, second, size) == HOS_OK);
    CHECK(hos_close_shared_memory(copy) && !hos_close_shared_memory(copy));
    MEMORY_BASIC_INFORMATION mi;
    CHECK(VirtualQuery((void *)(uintptr_t)first, &mi, sizeof mi) && mi.State == MEM_FREE);
}

static void test_read_only_and_cleanup(void) {
    const uint64_t addr = 0x3900023000ull, before = 0x3900010000ull, after = 0x3900070000ull;
    CHECK(rt_map_fixed(before, 0x1000) == 0 && rt_map_fixed(after, 0x1000) == 0);
    *(uint32_t *)(uintptr_t)before = 42; *(uint32_t *)(uintptr_t)after = 99;
    uint32_t handle = hos_new_shared_memory(0x40000, 1);
    CHECK(handle);
    CHECK(hos_map_shared_memory(handle, addr, 0x40000, 3) == HOS_KERNEL(108));
    CHECK(hos_map_shared_memory(handle, addr, 0x40000, 1) == HOS_OK);
    MEMORY_BASIC_INFORMATION mi;
    CHECK(VirtualQuery((void *)(uintptr_t)addr, &mi, sizeof mi) && mi.Protect == PAGE_READONLY);
    volatile uint8_t *host = hos_shared_memory_data(handle, NULL);
    host[0x3ffff] = 0xa5;
    CHECK(*(volatile uint8_t *)(uintptr_t)(addr + 0x3ffff) == 0xa5);
    CHECK(*(uint32_t *)(uintptr_t)before == 42 && *(uint32_t *)(uintptr_t)after == 99);
    CHECK(hos_unmap_shared_memory(handle, addr, 0x40000) == HOS_OK);
    CHECK(hos_map_shared_memory(handle, before, 0x40000, 1) == HOS_KERNEL(106));
    CHECK(*(uint32_t *)(uintptr_t)before == 42);
    CHECK(hos_close_shared_memory(handle));
    rt_unmap(before); rt_unmap(after);
    for (int i = 0; i < 100; i++) {
        handle = hos_new_shared_memory(0x1000, 1);
        CHECK(handle && hos_map_shared_memory(handle, first, 0x1000, 1) == HOS_OK);
        CHECK(hos_unmap_shared_memory(handle, first, 0x1000) == HOS_OK);
        CHECK(hos_close_shared_memory(handle));
    }
    CHECK(VirtualQuery((void *)(uintptr_t)first, &mi, sizeof mi) && mi.State == MEM_FREE);
}

static uint8_t msg[0x100];
static void put32(int off, uint32_t value) { memcpy(msg + off, &value, 4); }
static uint32_t get32(int off) { uint32_t value; memcpy(&value, msg + off, 4); return value; }

static void hid_request(uint32_t cmd, uint64_t a, uint64_t b, uint64_t d) {
    memset(msg, 0, sizeof msg);
    put32(0, 4); put32(4, 0x8000000d); put32(8, 1);
    put32(0x20, 0x49434653); put32(0x28, cmd);
    memcpy(msg + 0x30, &a, 8); memcpy(msg + 0x38, &b, 8); memcpy(msg + 0x40, &d, 8);
}

static uint32_t send_hid(Cpu *cpu, uint32_t session) {
    CHECK(hos_send_sync_request(cpu, session, msg, sizeof msg) == HOS_OK);
    return get32(0x18);
}

/* SendVibrationValue exactly as nn::hid packs it: handle at +0, VibrationValue at +4, aruid at +0x18 (32 bytes). */
static void vibration_request(uint32_t cmd, uint32_t handle, const float value[4]) {
    memset(msg, 0, sizeof msg);
    put32(0, 4); put32(4, 0x8000000f); put32(8, 1);
    put32(0x20, 0x49434653); put32(0x28, cmd);
    put32(0x30, handle);
    if (value) memcpy(msg + 0x34, value, 16);
    uint64_t aruid = 0x10; memcpy(msg + 0x48, &aruid, 8);
}
/* SendVibrationValues with the SDK's two X (pointer) descriptors: handles[] and VibrationValue[]. */
static void put_x(int off, const void *buffer, uint32_t size) {
    uint64_t addr = (uint64_t)(uintptr_t)buffer;
    put32(off, size << 16 | (uint32_t)((addr >> 32) & 15) << 12 | (uint32_t)((addr >> 36) & 7) << 6);
    put32(off + 4, (uint32_t)addr);
}
static void vibration_values_request(const uint32_t *handles, const float (*values)[4], unsigned count) {
    memset(msg, 0, sizeof msg);
    put32(0, 0x20004); put32(4, 0x80000009); put32(8, 1);
    put_x(0x14, handles, count * 4); put_x(0x1c, values, count * 16);
    put32(0x30, 0x49434653); put32(0x38, 206);
    uint64_t aruid = 0x10; memcpy(msg + 0x40, &aruid, 8);
}
static int amplitude_is(unsigned player, float low, float high) {
    float l, h; hos_hid_vibration_amplitude(player, &l, &h);
    return l == low && h == high;
}

static void test_hid_vibration(Cpu *cpu, uint32_t session) {
    const float neutral[4] = {0.f, 160.f, 0.f, 320.f}, strong[4] = {0.5f, 160.f, 0.25f, 320.f};
    const uint32_t no1_left = 0x00000001, no1_right = 0x00010001, no2_left = 0x00000101, no3_left = 0x00000201;
    /* The neutral value must stop the motors: reading it 4 bytes late took 160 Hz and 320 Hz as full amplitude. */
    vibration_request(201, no1_left, neutral); CHECK(send_hid(cpu, session) == HOS_OK);
    CHECK(amplitude_is(0, 0.f, 0.f));
    vibration_request(201, no1_left, strong); CHECK(send_hid(cpu, session) == HOS_OK);
    CHECK(amplitude_is(0, 0.5f, 0.25f));
    /* Other pads and the other motor do not cancel player 1's vibration. */
    vibration_request(201, no3_left, neutral); CHECK(send_hid(cpu, session) == HOS_OK);
    vibration_request(201, no1_right, neutral); CHECK(send_hid(cpu, session) == HOS_OK);
    vibration_request(201, no2_left, neutral); CHECK(send_hid(cpu, session) == HOS_OK);
    CHECK(amplitude_is(0, 0.5f, 0.25f) && amplitude_is(1, 0.f, 0.f));
    vibration_request(202, no1_left, NULL); CHECK(send_hid(cpu, session) == HOS_OK && !memcmp(msg + 0x20, strong, 16));
    vibration_request(202, no1_right, NULL); CHECK(send_hid(cpu, session) == HOS_OK && !memcmp(msg + 0x20, neutral, 16));
    /* PermitVibration(false) silences everything; true restores what the game last sent. */
    hid_request(204, 0, 0, 0); CHECK(send_hid(cpu, session) == HOS_OK && amplitude_is(0, 0.f, 0.f));
    hid_request(204, 1, 0, 0); CHECK(send_hid(cpu, session) == HOS_OK && amplitude_is(0, 0.5f, 0.25f));
    /* A batch through X descriptors updates every handle in it. X addresses have 39 bits, like guest memory. */
    uint8_t *batch = VirtualAlloc((void *)(uintptr_t)0x3a00000000ull, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(batch != NULL);
    if (!batch) return;
    uint32_t *handles = (uint32_t *)batch;
    float (*values)[4] = (float (*)[4])(batch + 0x100);
    handles[0] = no1_left; handles[1] = no1_right; handles[2] = no2_left;
    memcpy(values[0], neutral, 16); memcpy(values[1], neutral, 16); memcpy(values[2], strong, 16);
    vibration_values_request(handles, (const float (*)[4])values, 3); CHECK(send_hid(cpu, session) == HOS_OK);
    CHECK(amplitude_is(0, 0.f, 0.f) && amplitude_is(1, 0.5f, 0.25f));
    values[2][0] = values[2][2] = 0.f;
    vibration_values_request(handles, (const float (*)[4])values, 3); CHECK(send_hid(cpu, session) == HOS_OK);
    CHECK(amplitude_is(1, 0.f, 0.f));
    VirtualFree(batch, 0, MEM_RELEASE);
}

static void test_hid_ipc(void) {
    Cpu cpu = {0};
    Service hid = {"hid", hos_hid_handle, NULL};
    uint32_t session = hos_new_session(&hid);
    memset(msg, 0, sizeof msg);
    put32(0, 4); put32(4, 0x8000000b); put32(8, 1); /* PID descriptor */
    put32(0x20, 0x49434653); put32(0x30, 0x10000000);
    CHECK(hos_send_sync_request(&cpu, session, msg, sizeof msg) == HOS_OK && get32(0x18) == HOS_OK);
    uint32_t applet = get32(12);
    CHECK(applet);
    memset(msg, 0, sizeof msg);
    put32(0, 4); put32(4, 8); put32(0x10, 0x49434653);
    CHECK(hos_send_sync_request(&cpu, applet, msg, sizeof msg) == HOS_OK && get32(0x18) == HOS_OK);
    CHECK(get32(8) == 2); /* exactly one copy handle, no move handles */
    uint32_t shared = get32(12);
    uint64_t size = 0;
    uint8_t *host = hos_shared_memory_data(shared, &size);
    CHECK(host && size == 0x40000);
    CHECK(hos_map_shared_memory(shared, first, size, 1) == HOS_OK);
    host[0x123] = 77;
    CHECK(*(volatile uint8_t *)(uintptr_t)(first + 0x123) == 77);
    hid_request(103, 0x10, 0, 0);
    CHECK(send_hid(&cpu, session) == HOS_OK);
    const uint8_t *guest = (const uint8_t *)(uintptr_t)first;
    uint64_t capacity, count;
    memcpy(&capacity, guest + 0x9a00 + 0x28 + 8, 8);
    memcpy(&count, guest + 0x9a00 + 0x28 + 24, 8);
    CHECK(capacity == 17); /* valid ring capacity */
    hid_request(100, 0x1f, 0x10, 0); CHECK(send_hid(&cpu, session) == HOS_OK);
    hid_request(101, 0x10, 0, 0); CHECK(send_hid(&cpu, session) == HOS_OK && get32(0x20) == 0x1f);
    hid_request(120, 0x10, 1, 0); CHECK(send_hid(&cpu, session) == HOS_OK);
    hid_request(121, 0x10, 0, 0); CHECK(send_hid(&cpu, session) == HOS_OK && get32(0x20) == 1);
    hid_request(120, 0x10, 2, 0); CHECK(send_hid(&cpu, session) != HOS_OK);
    hid_request(121, 0x10, 0, 0); CHECK(send_hid(&cpu, session) == HOS_OK && get32(0x20) == 1);
    hid_request(128, 0x10, 2, 0); CHECK(send_hid(&cpu, session) == HOS_OK);
    hid_request(129, 0x10, 0, 0); CHECK(send_hid(&cpu, session) == HOS_OK && get32(0x20) == 2);
    hid_request(122, 0x20, 0x10, 0); CHECK(send_hid(&cpu, session) == HOS_OK);
    uint32_t assignment;
    memcpy(&assignment, guest + 0x9a00 + 8 * 0x5000 + 4, 4);
    CHECK(assignment == 1);
    memcpy(&assignment, guest + 0x9a00 + 9 * 0x5000 + 4, 4);
    CHECK(assignment == 0); /* Handheld 0x20 and Other 0x10 must not be swapped */
    hid_request(106, 0, 0x10, 0); CHECK(send_hid(&cpu, session) == HOS_OK && get32(8) == 2);
    uint32_t event = get32(12);
    hid_request(106, 0, 0x10, 0); CHECK(send_hid(&cpu, session) == HOS_OK);
    uint32_t event_copy = get32(12);
    CHECK(event && event_copy != event && hos_event_state(event) == 1);
    hos_event_set(event, 0);
    CHECK(hos_event_state(event_copy) == 0);
    hos_close_handle(event);
    CHECK(hos_event_state(event_copy) == 0);
    hos_close_handle(event_copy);
    hid_request(203, 0, 0, 0); CHECK(send_hid(&cpu, session) == HOS_OK);
    uint32_t vibration = get32(12);
    hid_request(0, 0x01000003, 0, 0); CHECK(send_hid(&cpu, vibration) == HOS_OK);
    hos_close_handle(vibration);
    test_hid_vibration(&cpu, session);
    CHECK(hos_unmap_shared_memory(shared, first, size) == HOS_OK);
    CHECK(hos_close_handle(shared) == HOS_OK && !hos_shared_memory_data(shared, NULL));
    hos_close_handle(applet); hos_close_handle(session);
}

static void test_mapping_outlives_all_handles(void) {
    const uint64_t addr = 0x3900081000ull;
    uint32_t handle = hos_new_shared_memory(0x1000, 3);
    CHECK(handle && hos_map_shared_memory(handle, addr, 0x1000, 3) == HOS_OK);
    volatile uint32_t *host = hos_shared_memory_data(handle, NULL);
    CHECK(hos_close_shared_memory(handle));
    /* With no handle left the guest cannot issue UnmapSharedMemory, but the mapping must remain valid until
     * process exit. Its reference keeps both the section and the service-side view alive. */
    host[0] = 1234;
    CHECK(*(volatile uint32_t *)(uintptr_t)addr == 1234);
    *(volatile uint32_t *)(uintptr_t)addr = 5678;
    CHECK(host[0] == 5678);
}

int main(void) {
    test_views();
    test_read_only_and_cleanup();
    test_hid_ipc();
    test_mapping_outlives_all_handles();
    printf("shared memory: %s (real aliases, read-only view, 4 KiB offsets, handles, remapping, HID IPC)\n",
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
