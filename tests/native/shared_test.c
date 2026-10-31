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
