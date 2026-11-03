/* Bindings resolved from the application's SDK exports by the generator (layout.c). No SDK code is embedded here.
 * A zero `vtable` means the application has no NintendoSDK movie library and the overrides are inactive. */
#ifndef DKR_MOVIE_SDK_H
#define DKR_MOVIE_SDK_H
#include "dkr/rt.h"

typedef struct DkrMovieSdk {
    uint64_t vtable;                                   /* _ZTVN5movie7DecoderE (the vptr is vtable + 0x10) */
    uint64_t signal_event;                             /* nn::os::SignalEvent(EventType *) */
    uint64_t buffer_set_data, buffer_set_range;        /* movie::Buffer::SetDataAndCapacity(void *, size_t), SetRange(int, int) */
    uint64_t buffer_data, buffer_capacity, buffer_size;
    uint64_t media_set_int32, media_set_rect;          /* movie::MediaData::SetInt32(const char *, int), SetRect(key, l, t, r, b) */
    uint64_t media_find_int32, media_find_buffer;      /* FindInt32(key, int *), FindBuffer(key, Buffer **) */
} DkrMovieSdk;
extern const DkrMovieSdk dkr_movie_sdk;

/* movie::Decoder member functions, replaced by runtime/src/movie.c (x0 = this, C++ ABI). */
void dkr_movie_ctor(Cpu *c);
void dkr_movie_ctor_priority(Cpu *c);
void dkr_movie_dtor(Cpu *c);
void dkr_movie_dtor_delete(Cpu *c);
void dkr_movie_configure(Cpu *c);
void dkr_movie_register_events(Cpu *c);
void dkr_movie_start(Cpu *c);
void dkr_movie_stop(Cpu *c);
void dkr_movie_flush(Cpu *c);
void dkr_movie_reset(Cpu *c);
void dkr_movie_last_error(Cpu *c);
void dkr_movie_acquire_input(Cpu *c);
void dkr_movie_get_input(Cpu *c);
void dkr_movie_send_input(Cpu *c);
void dkr_movie_acquire_output(Cpu *c);
void dkr_movie_output_size(Cpu *c);
void dkr_movie_output_format(Cpu *c);
void dkr_movie_output_buffer_format(Cpu *c);
void dkr_movie_get_output(Cpu *c);
void dkr_movie_get_output_format(Cpu *c);
void dkr_movie_release_output(Cpu *c);
#endif
