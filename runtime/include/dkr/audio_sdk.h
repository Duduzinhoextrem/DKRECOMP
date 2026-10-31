/* Bindings resolved from the application's SDK exports by the generator. No SDK code is embedded here. */
#ifndef DKR_AUDIO_SDK_H
#define DKR_AUDIO_SDK_H
#include "dkr/rt.h"

typedef struct DkrAudioSdk {
    uint64_t execute, lock, unlock, destroy_event, attach_event;
    uint64_t create_thread, start_thread, wait_thread, destroy_thread;
} DkrAudioSdk;
extern const DkrAudioSdk dkr_audio_sdk;
void dkr_audio_register(void);
void dkr_audio_work_size(Cpu *c);
void dkr_audio_open(Cpu *c);
void dkr_audio_open_event(Cpu *c);
void dkr_audio_close(Cpu *c);
void dkr_audio_execute(Cpu *c);

/* Decode the SDK 4.4 DeviceSink's processed mix buffers into interleaved stereo. */
int dkr_audio_capture_sdk4(const void *commands, size_t length, int16_t *out, size_t capacity);
#endif
