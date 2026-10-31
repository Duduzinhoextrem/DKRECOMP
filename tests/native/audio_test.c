/* AudioRenderer service sizing through CMIF, including malformed parameters and a measured service fixture.
 * REV4 0x308000 fixture: nxnative t_audren_work.c @ 5474c6f3, MIT, Copyright (c) 2026 seedds.
 * License: runtime/third_party/LICENSE-nxnative.txt. No renderer or audio data from a game is embedded. */
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

int main(void) {
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
    printf("AudioRenderer sizing: %s (REV4 service fixture, bounds, revision, padding, IPC)\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
