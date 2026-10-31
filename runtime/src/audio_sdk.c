/* CPU/manual audio backend using the renderer supplied by the application's SDK.
 * Adapted from nxnative dm/host/hle_audren.c @ 5474c6f34789a41770868564fbf4e9e32fc57584,
 * Copyright (c) 2026 seedds, MIT. See ../third_party/LICENSE-nxnative.txt and THIRD_PARTY_NOTICES.md.
 * SDK routines are recompiled locally; none of their code or data is distributed in this source. */
#include "hos.h"
#include "dkr/audio_sdk.h"
#include <stdatomic.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

enum { PARAM_SIZE = 0x38, FRAME_MAX = 1024, OUTPUT_BUFFERS = 4 };
static _Thread_local int t_in_open;
static _Atomic int g_open;
static uint64_t g_handle[2], g_owned_work, g_thread_memory, g_worker_entry;
static unsigned g_samples;
static SRWLOCK g_render_lock = SRWLOCK_INIT;
static int16_t g_frame[FRAME_MAX * 2];
static int g_frame_samples;
static uint64_t g_frames, g_nonsilent;

/* Calls use the current guest thread's actual registers/TLR. Preserve the surrounding host hook's arguments. */
static uint64_t call(Cpu *c, uint64_t address, unsigned n, const uint64_t *args) {
    Cpu saved = *c;
    for (unsigned i = 0; i < n; i++) c->x[i] = args[i];
    rt_call(c, address);
    uint64_t result = c->x[0];
    *c = saved;
    return result;
}

static uint64_t cpu_parameter(Cpu *c, uint64_t parameter) {
    c->sp -= 0x40;
    memcpy((void *)(uintptr_t)c->sp, (const void *)(uintptr_t)parameter, PARAM_SIZE);
    st32(c->sp + 0x2c, 1);
    st32(c->sp + 0x30, 1);
    return c->sp;
}

static uint64_t service_size(uint64_t parameter) {
    uint8_t internal[0x34] = {0};
    memcpy(internal, (const void *)(uintptr_t)parameter, 0x2c);
    internal[0x22] = internal[0x23] = 0;
    st32((uint64_t)(uintptr_t)internal + 0x30, ld32(parameter + 0x34));
    uint64_t result = 0;
    return hos_audio_work_size(internal, sizeof internal, &result) ? 0 : result;
}

void dkr_audio_work_size(Cpu *c) {
    if (t_in_open) { rt_call_original(c, "dkr_audio_work_size"); return; }
    uint64_t parameter = c->x[0], stack = c->sp;
    c->x[0] = cpu_parameter(c, parameter);
    rt_call_original(c, "dkr_audio_work_size");
    uint64_t cpu_size = c->x[0], guest_size = ld32(parameter + 0x2c) == 0 ? service_size(parameter) : 0;
    c->sp = stack;
    c->x[0] = guest_size ? guest_size : cpu_size;
    hos_log("audio: work buffer guest %#llx, SDK CPU/manual %#llx\n",
            (unsigned long long)c->x[0], (unsigned long long)cpu_size);
}

void dkr_audio_execute(Cpu *c) {
    /* SDK 4.4 AudioRenderSystem is Impl+0x60; Update locks os::MutexType at System+0x1d0.
     * Serialize command generation, processing and capture against both Update and other render callers. */
    uint64_t system = c->x[0] + 0x60, args[] = {system + 0x1d0};
    AcquireSRWLockExclusive(&g_render_lock);
    call(c, dkr_audio_sdk.lock, 1, args);
    rt_call_original(c, "dkr_audio_execute");
    uint64_t result = c->x[0];
    if (!result) {
        g_frame_samples = dkr_audio_capture_sdk4((const void *)(uintptr_t)ld64(system + 0x70),
                                               (size_t)ld64(system + 0x78), g_frame, FRAME_MAX * 2);
        if (g_frame_samples) {
            int loud = 0;
            for (int i = 0; i < g_frame_samples * 2; i++) loud |= g_frame[i];
            g_frames++;
            g_nonsilent += loud != 0;
            if (g_frames == 1 || g_frames == 200 || !(g_frames % 12000))
                hos_log("audio: captured %llu SDK frames, %llu non-silent, %d samples/frame\n",
                        (unsigned long long)g_frames, (unsigned long long)g_nonsilent, g_frame_samples);
        }
    }
    call(c, dkr_audio_sdk.unlock, 1, args);
    ReleaseSRWLockExclusive(&g_render_lock);
    c->x[0] = result;
}

static void render_frame(Cpu *c, int16_t *out) {
    uint64_t args[] = {g_handle[0], g_handle[1]};
    uint32_t result = (uint32_t)call(c, dkr_audio_sdk.execute, 2, args);
    if (result) hos_stop(c, "SDK manual audio rendering failed %#x", result);
    AcquireSRWLockExclusive(&g_render_lock);
    memset(out, 0, g_samples * 4);
    if ((unsigned)g_frame_samples == g_samples) memcpy(out, g_frame, g_samples * 4);
    ReleaseSRWLockExclusive(&g_render_lock);
}

static void render_main(Cpu *c) {
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    HWAVEOUT device = NULL;
    WAVEFORMATEX format = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
    int null_output = getenv("DKR_AUDIO_NULL") != NULL;
    MMRESULT error = null_output ? MMSYSERR_NODRIVER : waveOutOpen(&device, WAVE_MAPPER, &format,
                                         (DWORD_PTR)event, 0, CALLBACK_EVENT);
    int16_t pcm[OUTPUT_BUFFERS][FRAME_MAX * 2] = {{0}};
    WAVEHDR headers[OUTPUT_BUFFERS] = {{0}};
    if (error) {
        hos_log("audio: %s (WinMM result %u); rendering with a timer, no host sound\n",
                null_output ? "null output requested" : "output device unavailable", error);
        while (atomic_load(&g_open)) {
            render_frame(c, pcm[0]);
            kern_lock(); kern_sleep((int64_t)g_samples * 1000000000 / 48000); kern_unlock();
        }
    } else {
        hos_log("audio: WinMM output opened, 48 kHz stereo, %u x %u samples\n", OUTPUT_BUFFERS, g_samples);
        waveOutPause(device);
        for (unsigned i = 0; i < OUTPUT_BUFFERS; i++) {
            headers[i].lpData = (LPSTR)pcm[i];
            headers[i].dwBufferLength = g_samples * 4;
            if (waveOutPrepareHeader(device, &headers[i], sizeof headers[i]))
                hos_stop(c, "cannot prepare audio output buffer");
        }
        unsigned index = 0, queued = 0;
        while (atomic_load(&g_open)) {
            WAVEHDR *header = &headers[index];
            ResetEvent(event);
            if ((header->dwFlags & WHDR_INQUEUE) && !(header->dwFlags & WHDR_DONE)) {
                WaitForSingleObject(event, 10);
                continue;
            }
            render_frame(c, pcm[index]);
            if (waveOutWrite(device, header, sizeof *header)) hos_stop(c, "cannot submit audio output buffer");
            if (++queued == OUTPUT_BUFFERS) waveOutRestart(device);
            index = (index + 1) % OUTPUT_BUFFERS;
        }
        waveOutReset(device);
        for (unsigned i = 0; i < OUTPUT_BUFFERS; i++) waveOutUnprepareHeader(device, &headers[i], sizeof headers[i]);
        waveOutClose(device);
    }
    if (event) CloseHandle(event);
}

void dkr_audio_register(void) { g_worker_entry = rt_hostfn_register("SDK audio render thread", render_main); }

static void open_renderer(Cpu *c, int with_event) {
    const char *original = with_event ? "dkr_audio_open_event" : "dkr_audio_open";
    if (t_in_open) { rt_call_original(c, original); return; }
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_open, &expected, 1)) { c->x[0] = 0x499; return; }
    Cpu saved = *c;
    uint64_t handle = c->x[0], event = with_event ? c->x[1] : 0;
    unsigned p = with_event ? 2 : 1;
    uint64_t parameter = c->x[p], work = c->x[p + 1], size = c->x[p + 2];
    int dsp = ld32(parameter + 0x2c) == 0;
    uint64_t converted = cpu_parameter(c, parameter);
    uint64_t guest_need = dsp ? service_size(parameter) : 0;
    c->x[0] = converted;
    rt_call_original(c, "dkr_audio_work_size");
    uint64_t need = c->x[0];
    if (dsp && guest_need && size >= guest_need && !(work & 0xfff) && !(size & 0xfff) && need > size) {
        g_owned_work = hos_backend_alloc(need);
        if (!g_owned_work) hos_stop(c, "cannot allocate SDK audio backend work buffer");
        work = g_owned_work;
        size = need;
    }
    *c = saved;
    c->sp = converted;
    c->x[p] = converted; c->x[p + 1] = work; c->x[p + 2] = size;
    t_in_open = 1;
    rt_call_original(c, original);
    t_in_open = 0;
    uint32_t result = (uint32_t)c->x[0];
    *c = saved;
    c->x[0] = result;
    if (result) {
        hos_backend_free(g_owned_work); g_owned_work = 0;
        atomic_store(&g_open, 0);
        hos_log("audio: SDK CPU/manual OpenAudioRenderer failed %#x\n", result);
        return;
    }
    if (event && dsp) {
        uint64_t destroy[] = {event};
        call(c, dkr_audio_sdk.destroy_event, 1, destroy);
        kern_lock();
        uint32_t writable = hos_new_event(0), readable = hos_copy_event_handle(writable);
        kern_unlock();
        if (!writable || !readable) hos_stop(c, "cannot allocate the audio renderer kernel event");
        uint64_t attach[] = {event, writable, 1, readable, 1, 1};
        call(c, dkr_audio_sdk.attach_event, 6, attach);
    }
    memcpy(g_handle, (const void *)(uintptr_t)handle, sizeof g_handle);
    g_samples = ld32(parameter + 4);
    if (!g_samples || g_samples > FRAME_MAX || ld32(parameter) != 48000)
        hos_stop(c, "unsupported SDK audio output format");
    g_frames = g_nonsilent = 0; g_frame_samples = 0;
    g_thread_memory = hos_backend_alloc(0x20000);
    if (!g_thread_memory) hos_stop(c, "cannot allocate audio render thread");
    uint64_t create[] = {g_thread_memory, g_worker_entry, 0, g_thread_memory + 0x10000, 0x10000, 9, 0};
    result = (uint32_t)call(c, dkr_audio_sdk.create_thread, 7, create);
    if (result) hos_stop(c, "SDK audio thread creation failed %#x", result);
    uint64_t start[] = {g_thread_memory};
    call(c, dkr_audio_sdk.start_thread, 1, start);
    hos_log("audio: SDK CPU/manual renderer opened; guest work %#llx, backend %#llx, kernel event %s\n",
            (unsigned long long)saved.x[p + 2], (unsigned long long)need, event && dsp ? "yes" : "no");
    c->x[0] = 0;
}

void dkr_audio_open(Cpu *c) { open_renderer(c, 0); }
void dkr_audio_open_event(Cpu *c) { open_renderer(c, 1); }

void dkr_audio_close(Cpu *c) {
    if (c->x[0] == g_handle[0] && g_thread_memory) {
        atomic_store(&g_open, 0);
        uint64_t args[] = {g_thread_memory};
        call(c, dkr_audio_sdk.wait_thread, 1, args);
        call(c, dkr_audio_sdk.destroy_thread, 1, args);
        hos_backend_free(g_thread_memory); g_thread_memory = 0;
    }
    rt_call_original(c, "dkr_audio_close");
    hos_backend_free(g_owned_work); g_owned_work = 0;
    memset(g_handle, 0, sizeof g_handle);
}
