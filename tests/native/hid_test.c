/* Nintendo Switch Firmware 4.x Npad Shared Memory & Host Input Unit Test.
 * Validates Npad slots (Player 1 slot 0, Handheld slot 8), RingLifo headers,
 * 200 Hz sample counter advancement, seqlock atomic storage integrity,
 * and button/stick mappings. */
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

/* 4.x ABI definitions */
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

_Static_assert(sizeof(NpadCommonState) == 0x28, "NpadCommonState ABI");
_Static_assert(sizeof(NpadCommonStateAtomicStorage) == 0x30, "NpadCommonStateAtomicStorage ABI");
_Static_assert(sizeof(NpadCommonLifo) == 0x350, "NpadCommonLifo ABI");

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

static const uint64_t guest_addr = 0x3900001000ull;

static void test_npad_layout_and_input(void) {
    Cpu cpu = {0};
    Service hid = {"hid", hos_hid_handle, NULL};
    uint32_t session = hos_new_session(&hid);
    CHECK(session != 0);

    // CreateAppletResource
    memset(msg, 0, sizeof msg);
    put32(0, 4); put32(4, 0x8000000b); put32(8, 1);
    put32(0x20, 0x49434653); put32(0x30, 0x10000000);
    CHECK(hos_send_sync_request(&cpu, session, msg, sizeof msg) == HOS_OK && get32(0x18) == HOS_OK);
    uint32_t applet = get32(12);
    CHECK(applet != 0);

    // GetSharedMemoryHandle
    memset(msg, 0, sizeof msg);
    put32(0, 4); put32(4, 8); put32(0x10, 0x49434653);
    CHECK(hos_send_sync_request(&cpu, applet, msg, sizeof msg) == HOS_OK && get32(0x18) == HOS_OK);
    uint32_t shared = get32(12);
    CHECK(shared != 0);

    uint64_t shmem_size = 0;
    uint8_t *host_mem = hos_shared_memory_data(shared, &shmem_size);
    CHECK(host_mem != NULL && shmem_size == 0x40000);

    CHECK(hos_map_shared_memory(shared, guest_addr, shmem_size, 1) == HOS_OK);
    const uint8_t *guest = (const uint8_t *)(uintptr_t)guest_addr;

    // Set supported styles: FullKey (1), Handheld (2), JoyDual (4)
    hid_request(100, 0x7, 0x10, 0);
    CHECK(send_hid(&cpu, session) == HOS_OK);

    // Activate Npad
    hid_request(103, 0x10, 0, 0);
    CHECK(send_hid(&cpu, session) == HOS_OK);

    // Verify Slot 0 (Player 1) at offset 0x9A00
    const uint8_t *slot0 = guest + 0x9a00;
    uint32_t style0 = 0;
    memcpy(&style0, slot0 + 0, 4);
    CHECK((style0 & 1) != 0); // FullKey supported/connected

    const NpadCommonLifo *full_key_lifo = (const NpadCommonLifo *)(slot0 + 0x28);
    CHECK(full_key_lifo->buffer_count == 17);
    CHECK(full_key_lifo->count >= 1);
    CHECK(full_key_lifo->tail < 17);

    uint64_t current_tail = full_key_lifo->tail;
    const NpadCommonStateAtomicStorage *entry0 = &full_key_lifo->storage[current_tail];
    CHECK(entry0->sampling_number > 0);
    CHECK(entry0->sampling_number == entry0->state.sampling_number);
    CHECK((entry0->state.attributes & 1) != 0); // IsConnected = 1

    // Verify Slot 8 (Handheld) at offset 0x9A00 + 8 * 0x5000 = 0x31A00
    const uint8_t *slot8 = guest + 0x9a00 + 8 * 0x5000;
    uint32_t style8 = 0;
    memcpy(&style8, slot8 + 0, 4);
    CHECK((style8 & 2) != 0); // Handheld supported/connected

    const NpadCommonLifo *handheld_lifo = (const NpadCommonLifo *)(slot8 + 0x28 + 1 * 0x350);
    CHECK(handheld_lifo->buffer_count == 17);
    CHECK(handheld_lifo->count >= 1);
    CHECK(handheld_lifo->tail < 17);

    const NpadCommonStateAtomicStorage *entry8 = &handheld_lifo->storage[handheld_lifo->tail];
    CHECK(entry8->sampling_number > 0);
    CHECK(entry8->sampling_number == entry8->state.sampling_number);
    CHECK((entry8->state.attributes & 1) != 0); // IsConnected = 1

    // Test input mapping: set synthetic buttons and sticks
    // A=0, B=1, Plus=10, ZL=8
    uint64_t test_btns = (1ull << 0) | (1ull << 1) | (1ull << 10) | (1ull << 8);
    int32_t test_lx = -22000, test_ly = 26000;
    int32_t test_rx = 15000, test_ry = -18000;
    hos_hid_set_test_input(test_btns, test_lx, test_ly, test_rx, test_ry);
    hos_hid_tick();

    current_tail = full_key_lifo->tail;
    const NpadCommonState *cur_state = &full_key_lifo->storage[current_tail].state;
    CHECK((cur_state->buttons & (1ull << 0)) != 0);  // A
    CHECK((cur_state->buttons & (1ull << 1)) != 0);  // B
    CHECK((cur_state->buttons & (1ull << 10)) != 0); // Plus
    CHECK((cur_state->buttons & (1ull << 8)) != 0);  // ZL
    // Stick deflection direction bits:
    CHECK((cur_state->buttons & (1ull << 16)) != 0); // StickL Left (< -16384)
    CHECK((cur_state->buttons & (1ull << 17)) != 0); // StickL Up (> 16384)
    CHECK((cur_state->buttons & (1ull << 23)) != 0); // StickR Down (< -16384)
    CHECK(cur_state->stick_l_x == test_lx);
    CHECK(cur_state->stick_l_y == test_ly);
    CHECK(cur_state->stick_r_x == test_rx);
    CHECK(cur_state->stick_r_y == test_ry);
    CHECK((cur_state->attributes & 1) == 1);

    hos_hid_clear_test_input();

    // Test ring advancement and wrap-around
    uint64_t sample_before = full_key_lifo->storage[full_key_lifo->tail].sampling_number;
    for (int step = 0; step < 25; step++) {
        hos_hid_tick();
    }
    uint64_t sample_after = full_key_lifo->storage[full_key_lifo->tail].sampling_number;
    uint64_t tail_after = full_key_lifo->tail;
    CHECK(sample_after >= sample_before + 25);
    CHECK(tail_after == (sample_after - 1) % 17);
    CHECK(full_key_lifo->count == 17);

    // Verify all 17 slots in the ring have consistent seqlock sampling numbers
    for (int i = 0; i < 17; i++) {
        const NpadCommonStateAtomicStorage *s = &full_key_lifo->storage[i];
        CHECK(s->sampling_number == s->state.sampling_number);
    }

    // Verify background thread is actively sampling (~200 Hz)
    uint64_t bg_start = full_key_lifo->storage[full_key_lifo->tail].sampling_number;
    Sleep(40); // 40 ms should produce ~5-9 ticks at 200 Hz
    uint64_t bg_end = full_key_lifo->storage[full_key_lifo->tail].sampling_number;
    CHECK(bg_end > bg_start);

    // Test DeactivateNpad (cmd 104)
    hid_request(104, 0, 0, 0);
    CHECK(send_hid(&cpu, session) == HOS_OK);

    Sleep(10); // allow any in-flight tick to finish
    uint64_t deact_sample = full_key_lifo->storage[full_key_lifo->tail].sampling_number;
    Sleep(25);
    uint64_t deact_after = full_key_lifo->storage[full_key_lifo->tail].sampling_number;
    CHECK(deact_after == deact_sample); // Must not advance while deactivated

    // Cleanup
    CHECK(hos_unmap_shared_memory(shared, guest_addr, shmem_size) == HOS_OK);
    CHECK(hos_close_handle(shared) == HOS_OK);
    CHECK(hos_close_handle(applet) == HOS_OK);
    CHECK(hos_close_handle(session) == HOS_OK);
}

int main(void) {
    test_npad_layout_and_input();
    printf("hid test: %s (Firmware 4.x layout, RingLifo, seqlock atomic storage, 200 Hz thread, mappings)\n",
           failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
