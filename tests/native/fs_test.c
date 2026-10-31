/* Exercise fsp-srv / IStorage through real IPC messages against a small synthetic host file. */
#include "../../runtime/src/hos.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; \
} } while (0)

void hos_log(const char *fmt, ...) { (void)fmt; }
void hos_stop(Cpu *c, const char *fmt, ...) {
    (void)c;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    exit(2);
}
Service *hos_get_service(const char *name) { (void)name; return NULL; }
void kern_sync_poke(uint32_t handle) { (void)handle; }

static uint8_t msg[0x100];
static Cpu cpu;
static const uint64_t guest_buffer = 0x3900000000ull;
static void put32(int off, uint32_t v) { memcpy(msg + off, &v, 4); }
static void put64(int off, uint64_t v) { memcpy(msg + off, &v, 8); }
static uint32_t get32(int off) { uint32_t v; memcpy(&v, msg + off, 4); return v; }
static uint64_t get64(int off) { uint64_t v; memcpy(&v, msg + off, 8); return v; }

static void request(uint32_t cmd) {
    memset(msg, 0, sizeof msg);
    put32(0, 4); put32(4, 8);
    put32(0x10, 0x49434653); put32(0x18, cmd);
}

static uint32_t read_storage(uint32_t handle, int64_t offset, int64_t length, uint32_t buffer_size) {
    memset(msg, 0, sizeof msg);
    put32(0, 0x01000004); put32(4, 11); /* one B descriptor, raw data ending at 0x40 */
    put32(8, buffer_size);
    put32(12, (uint32_t)guest_buffer);
    put32(16, ((uint32_t)(guest_buffer >> 32) & 15) << 28 |
              ((uint32_t)(guest_buffer >> 36) & 7) << 2); /* split upper address bits */
    put32(0x20, 0x49434653);
    put64(0x30, (uint64_t)offset); put64(0x38, (uint64_t)length);
    CHECK(hos_send_sync_request(&cpu, handle, msg, sizeof msg) == HOS_OK);
    return get32(0x18);
}

int main(void) {
    char temp_dir[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathA(sizeof temp_dir, temp_dir) || !GetTempFileNameA(temp_dir, "dkr", 0, path)) return 2;
    uint8_t fixture[256];
    for (int i = 0; i < sizeof fixture; i++) fixture[i] = (uint8_t)i;
    uint64_t header_size = 0x50;
    memcpy(fixture, &header_size, 8);
    FILE *file = fopen(path, "wb");
    if (!file) return 2;
    CHECK(fwrite(fixture, 1, sizeof fixture, file) == sizeof fixture);
    fclose(file);
    CHECK(rt_map_fixed(guest_buffer, 0x1000) == 0);
    uint8_t *out = (uint8_t *)(uintptr_t)guest_buffer;
    hos_fs_set_romfs(path);
    Service fsp = {"fsp-srv", hos_fsp_handle, NULL};
    uint32_t session = hos_new_session(&fsp);
    request(200);
    CHECK(hos_send_sync_request(&cpu, session, msg, sizeof msg) == HOS_OK);
    uint32_t storage = get32(12);
    CHECK(storage && get32(0x18) == HOS_OK);
    request(4);
    CHECK(hos_send_sync_request(&cpu, storage, msg, sizeof msg) == HOS_OK);
    CHECK(get64(0x20) == sizeof fixture);
    memset(out, 0xee, 0x1000);
    CHECK(read_storage(storage, 0, 80, 80) == HOS_OK);
    CHECK(!memcmp(out, fixture, 80) && out[80] == 0xee);
    CHECK(read_storage(storage, 240, 16, 16) == HOS_OK);
    CHECK(!memcmp(out, fixture + 240, 16));
    CHECK(read_storage(storage, 256, 0, 0) == HOS_OK);

    memset(out, 0xee, 0x1000);
    CHECK(read_storage(storage, -1, 1, 1) != HOS_OK);
    CHECK(read_storage(storage, 0, -1, 1) != HOS_OK);
    CHECK(read_storage(storage, 255, 2, 2) != HOS_OK);
    CHECK(read_storage(storage, INT64_MAX, INT64_MAX, 16) != HOS_OK);
    CHECK(read_storage(storage, 0, 32, 16) != HOS_OK);
    for (int i = 0; i < 0x1000; i++) CHECK(out[i] == 0xee);
    hos_close_handle(storage);
    hos_close_handle(session);
    hos_fs_set_romfs(""); /* closes the image so Windows can remove the fixture */
    CHECK(DeleteFileA(path));
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
