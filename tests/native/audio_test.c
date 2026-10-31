/* AudioRenderer service sizing through CMIF, including malformed parameters and a measured service fixture.
 * REV4 0x308000 fixture: nxnative t_audren_work.c @ 5474c6f3, MIT, Copyright (c) 2026 seedds.
 * License: runtime/third_party/LICENSE-nxnative.txt. No renderer or audio data from a game is embedded. */
#include "../../runtime/src/hos.h"
#include "dkr/audio_sdk.h"
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

static void test_device_sink(void) {
    uint8_t commands[0x160] = {0};
    uint64_t base = (uint64_t)(uintptr_t)commands, sink = base + 0x20;
    int32_t mix[6][4] = {{40000, -40000, 7, -8}, {-32000, 32000, -9, 10}};
    int16_t pcm[8] = {0};
    st32(base + 4, 1); st64(base + 8, (uint64_t)(uintptr_t)mix);
    st32(base + 0x10, 6); st32(base + 0x14, 4); st32(base + 0x18, 48000);
    st32(sink, 0xcafebabe); st16(sink + 4, 0x130); st16(sink + 6, 0xf); st8(sink + 0x10, 1);
    st32(sink + 0x120, 2); st8(sink + 0x124, 0); st8(sink + 0x125, 1);
    CHECK(dkr_audio_capture_sdk4(commands, sizeof commands, pcm, 8) == 4);
    CHECK(pcm[0] == 32767 && pcm[2] == -32768 && pcm[1] == -32000 && pcm[3] == 32000);
    CHECK(pcm[4] == 7 && pcm[5] == -9 && pcm[6] == -8 && pcm[7] == 10);
    CHECK(!dkr_audio_capture_sdk4(commands, sizeof commands, pcm, 7));
    CHECK(!dkr_audio_capture_sdk4(commands, 0x14f, pcm, 8));
    st8(sink + 0x125, 0xff);
    CHECK(!dkr_audio_capture_sdk4(commands, sizeof commands, pcm, 8));
    st8(sink + 0x125, 1); st8(sink + 0x10, 0);
    CHECK(!dkr_audio_capture_sdk4(commands, sizeof commands, pcm, 8));
    st8(sink + 0x10, 1); st32(base + 4, 2); /* malformed command after the valid sink */
    CHECK(!dkr_audio_capture_sdk4(commands, sizeof commands, pcm, 8));
    st32(base + 4, 1); st32(sink + 0x120, 6);
    for (unsigned i = 0; i < 6; i++) { st8(sink + 0x124 + i, i); mix[i][0] = 1000; }
    CHECK(dkr_audio_capture_sdk4(commands, sizeof commands, pcm, 8) == 4);
    CHECK(pcm[0] == 2664 && pcm[1] == 2664);
}

int main(void) {
    test_device_sink();
    const uint32_t fixture[13] = {48000, 240, 132, 19, 138, 1, 16, 0, 0, 0, 0, 0, 0x34564552};
    uint32_t parameter[13];
    memcpy(parameter, fixture, sizeof parameter);
    uint64_t size = 0;
    CHECK(hos_audio_work_size((uint8_t *)parameter, sizeof parameter, &size) == HOS_OK && size == 0x308000);
    uint8_t *bytes = (uint8_t *)parameter;
    bytes[0x20] = 1; bytes[0x21] = 0xff; /* voice-drop is one byte; public wrapper padding is not the flag */
    CHECK(hos_audio_work_size(bytes, sizeof parameter, &size) == HOS_OK && size == 0x308000);
    bytes[0x22] = 1; /* the current service supports DSP + automatic execution only */
    CHECK(hos_audio_work_size(bytes, sizeof parameter, &size) != HOS_OK);
    memcpy(parameter, fixture, sizeof parameter);
    size = 0x1234;
    CHECK(hos_audio_work_size(bytes, sizeof parameter - 1, &size) != HOS_OK && size == 0x1234);
    parameter[4] = 0xffffffff;
    CHECK(hos_audio_work_size(bytes, sizeof parameter, &size) != HOS_OK && size == 0x1234);
    memcpy(parameter, fixture, sizeof parameter);
    parameter[12] = 0x35564552;
    CHECK(hos_audio_work_size(bytes, sizeof parameter, &size) != HOS_OK);
    parameter[12] = 0x34564552; parameter[2] = 0x100001;
    CHECK(hos_audio_work_size(bytes, sizeof parameter, &size) != HOS_OK);
    parameter[2] = fixture[2]; parameter[1] = 0;
    CHECK(hos_audio_work_size(bytes, sizeof parameter, &size) != HOS_OK);
    memcpy(parameter, fixture, sizeof parameter);

    uint8_t msg[0x100] = {0};
    uint32_t header[] = {4, 19};
    memcpy(msg, header, sizeof header);
    uint32_t magic = 0x49434653, cmd = 1;
    memcpy(msg + 0x10, &magic, 4); memcpy(msg + 0x18, &cmd, 4);
    memcpy(msg + 0x20, parameter, sizeof parameter);
    Service audio = {"audren:u", hos_audren_handle, NULL};
    uint32_t session = hos_new_session(&audio);
    Cpu cpu = {0};
    CHECK(hos_send_sync_request(&cpu, session, msg, sizeof msg) == HOS_OK);
    uint32_t result;
    memcpy(&result, msg + 0x18, 4); memcpy(&size, msg + 0x20, 8);
    CHECK(result == HOS_OK && size == 0x308000 && (size & 0xfff) == 0);
    hos_close_handle(session);
    printf("AudioRenderer: %s (REV4 sizing/IPC; SDK 4 DeviceSink stereo, downmix, clipping, malformed lists)\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
