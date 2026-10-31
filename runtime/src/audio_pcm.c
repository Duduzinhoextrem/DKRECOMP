/* SDK 4.4 DeviceSink capture, adapted from nxnative capture(), MIT, Copyright (c) 2026 seedds.
 * Source: dm/host/hle_audren.c @ 5474c6f34789a41770868564fbf4e9e32fc57584.
 * See ../third_party/LICENSE-nxnative.txt. Only the measured 48 kHz SDK 4 layout is accepted here. */
#include "dkr/audio_sdk.h"

int dkr_audio_capture_sdk4(const void *commands, size_t length, int16_t *out, size_t capacity) {
    const uint8_t *cl = commands;
    if (!cl || !out || length < 0x20) return 0;
    uint64_t base = (uint64_t)(uintptr_t)cl;
    uint32_t count = ld32(base + 4), samples = ld32(base + 0x14), buffers = ld32(base + 0x10);
    if (ld32(base + 0x18) != 48000 || !samples || samples > capacity / 2 || samples > 1024) return 0;
    const int32_t *mix = (const int32_t *)(uintptr_t)ld64(base + 8);
    if (!mix || !buffers) return 0;
    size_t offset = 0x20;
    const uint8_t *sink = NULL;
    /* Validate the entire list before producing output, including commands after the sink. */
    for (uint32_t i = 0; i < count; i++) {
        if (offset > length || length - offset < 0x14) return 0;
        const uint8_t *q = cl + offset;
        uint64_t p = (uint64_t)(uintptr_t)q;
        uint16_t size = ld16(p + 4), id = ld16(p + 6);
        if (ld32(p) != 0xcafebabe || size < 0x14 || size > length - offset) return 0;
        if (id == 0xf && size == 0x130 && q[0x10]) {
            if (sink) return 0; /* Multiple active device sinks need separate routing. */
            sink = q;
        }
        offset += size;
    }
    if (!sink) return 0;
    uint32_t channels = ld32((uint64_t)(uintptr_t)sink + 0x120);
    if (!channels || channels > 6) return 0;
    const int32_t *ch[6];
    for (unsigned j = 0; j < channels; j++) {
        int index = (int8_t)sink[0x124 + j];
        if (index < 0 || (unsigned)index >= buffers) return 0;
        ch[j] = mix + (size_t)index * samples;
    }
    for (unsigned i = 0; i < samples; i++) {
        int64_t left = ch[0][i], right = channels > 1 ? ch[1][i] : left;
        if (channels == 6) {
            int64_t center = (int64_t)ch[2][i] * 23167 + (int64_t)ch[3][i] * 8224;
            left = (left * 32768 + center + (int64_t)ch[4][i] * 23167) >> 15;
            right = (right * 32768 + center + (int64_t)ch[5][i] * 23167) >> 15;
        } else if (channels > 2) return 0;
        out[i * 2] = (int16_t)(left > 32767 ? 32767 : left < -32768 ? -32768 : left);
        out[i * 2 + 1] = (int16_t)(right > 32767 ? 32767 : right < -32768 ? -32768 : right);
    }
    return (int)samples;
}
