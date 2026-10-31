/* Initial AudioRenderer IPC. Work-buffer sizing is adapted from nxnative ar_service_work_size:
 * Copyright (c) 2026 seedds, MIT; see ../third_party/LICENSE-nxnative.txt and THIRD_PARTY_NOTICES.md.
 * Source: seedds/nxnative dm/host/hle_audren.c @ 5474c6f34789a41770868564fbf4e9e32fc57584.
 * Adaptation: internal firmware-4.x parameter layout, REV1-4 only, explicit validation and DKRecomp IPC.
 * Renderer/update/mixing commands remain explicit stops until their implementation exists. */
#include "hos.h"

#define AUDIO_INVALID_PARAMETER 0x499u

static uint32_t audio_u32(const uint8_t *p) { uint32_t value; memcpy(&value, p, 4); return value; }
static uint64_t aligned(uint64_t size, uint64_t alignment) { return (size + alignment - 1) & ~(alignment - 1); }

/* AudioRendererParameterInternal: 0x34 bytes, revision at +0x30 (not the public wrapper's +0x34).
 * Voice-drop at +0x20 is a byte. Device/mode at +0x22/+0x23 are transport flags, not part of voice-drop. */
uint32_t hos_audio_work_size(const uint8_t *parameter, uint32_t length, uint64_t *out) {
    if (!parameter || !out || length < 0x34) return AUDIO_INVALID_PARAMETER;
    uint32_t revision = audio_u32(parameter + 0x30);
    if ((revision & 0xffffff) != 0x564552 || revision < 0x31564552 || revision > 0x34564552)
        return AUDIO_INVALID_PARAMETER;
    unsigned r = (revision >> 24) - 0x30;
    uint64_t n[11];
    for (unsigned i = 0; i < 11; i++) {
        if (i == 8) { n[i] = parameter[0x20]; continue; }
        int32_t value = (int32_t)audio_u32(parameter + i * 4);
        if (value < 0 || value > 0x100000) return AUDIO_INVALID_PARAMETER;
        n[i] = (uint32_t)value;
    }
    if ((n[0] != 32000 && n[0] != 48000) || !n[1] || n[1] > 1024 || n[8] > 1 ||
        parameter[0x22] != 0 || parameter[0x23] != 0) return AUDIO_INVALID_PARAMETER;
    uint64_t mix = n[2], sub = n[3], voices = n[4], sinks = n[5], effects = n[6], perf = n[7];
    uint64_t splitters = n[9], destinations = n[10], mixes = sub + 1;
    if (r == 1 && (splitters || destinations)) return AUDIO_INVALID_PARAMETER;

    uint64_t size = aligned(((sinks + sub) * 0x3c0 + n[1] * 4) * (mix + 6), 64);
    size += aligned(mix * 4, 64) + sub * 0x400 + mixes * 0x940 + aligned(mixes * 8, 16);
    size += voices * 0x1d0 + aligned(voices * 0x220, 16) + aligned(voices * 8, 16);
    if (r >= 2) {
        uint64_t graph = mixes * mixes * 4 + mixes * 12 + 2 * aligned(mixes, 64) / 8 + aligned(mixes * mixes, 64) / 8;
        size += aligned(graph, 16);
        size += splitters * 0x20 + aligned(destinations * 0xe0, 16) + aligned(destinations * 4, 16);
    }
    size += (effects + voices * 4) * 0x20 + 0x50;
    size = aligned(size, 64);
    size += (sinks + sub) * 0x280 + effects * 0x4b0 + sinks * 0x170 + voices * 0x100 + 0x40;
    if (perf) {
        uint64_t frame = (sub + voices + sinks + effects + 1) * 0x10 + 0x658;
        size += aligned(frame * (perf + 1), 64) + 0xc0;
    }
    *out = aligned(size + 0x18000 + 0x7e, 0x1000);
    return HOS_OK;
}

void hos_audren_handle(Cpu *c, Service *s, const IpcRequest *rq, IpcReply *rp) {
    switch (rq->cmd) {
    case 1: { /* GetWorkBufferSize(AudioRendererParameterInternal) -> u64 */
        uint64_t size = 0;
        rp->result = hos_audio_work_size(rq->args, rq->args_size, &size);
        if (rp->result) return;
        hos_log("audio: REV%u work buffer %#llx, %u Hz/%u samples, %u voices\n",
                (audio_u32(rq->args + 0x30) >> 24) - 0x30, (unsigned long long)size,
                audio_u32(rq->args), audio_u32(rq->args + 4), audio_u32(rq->args + 0x10));
        RP_PUT(rp, uint64_t, size);
        return;
    }
    default: hos_unimplemented(c, s, rq);
    }
}
