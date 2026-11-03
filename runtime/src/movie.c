/* Host movie::Decoder for the NintendoSDK movie library (subsdk0 = NintendoSDK_movie 4.4.0).
 *
 * The SDK's Decoder wraps NVIDIA's hardware decoder (NvMMLite), which the host does not have: Tropical Freeze's
 * opening movie never started and the game waited on its loading screen for good. movie::Extractor (the SDK's MP4
 * demuxer, plain CPU code) is kept; every movie::Decoder member is replaced here. Video (H.264) is decoded to NV12
 * and audio (AAC) to interleaved PCM16 with Windows Media Foundation, part of Windows itself.
 *
 * The client protocol is Android's MediaCodec, which movie::Decoder wraps: input indices are acquired, filled through a
 * movie::Buffer and sent with a time in microseconds and flags (4 = end of stream); decoded outputs are acquired by
 * index, copied into a client buffer with GetOutputBuffer and released. The client's DecoderEvents hold four
 * nn::os::EventType pointers {input available, output available, format changed, error}, read when signalled.
 * Protocol, result codes, MediaData keys and slot counts follow nxnative's dm/host/hle_movie.c
 * @ aa9fa765944b00dd822c125d99d49d3e658a49b2, MIT, Copyright (c) 2026 seedds
 * (see ../third_party/LICENSE-nxnative.txt). The Media Foundation decoding is DKRecomp's own.
 *
 * Guest code (SDK helpers, SignalEvent) is only called on the calling guest thread and never while a host lock is held:
 * a woken guest thread may call back into the decoder. Guest threads have 8 MiB native stacks, enough for Media
 * Foundation, which each thread initializes for COM on first use. */
#define INITGUID
#define COBJMACROS
#include "hos.h"
#include "dkr/movie_sdk.h"
#include "dkr/settings.h"
#include "dkr/movie_interp.h"
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>

/* Declared by MIDL headers without definitions (they live in mfuuid/wmcodecdspuuid); defined here instead. */
static const GUID kClsidH264Decoder = {0x62CE7E72, 0x4C71, 0x4d20, {0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D}};
static const GUID kClsidAacDecoder = {0x32d186a7, 0x218f, 0x4c75, {0x88, 0x76, 0xdd, 0x77, 0x27, 0x3a, 0x89, 0x99}};
static const IID kIidTransform = {0xbf94c121, 0x5b05, 0x4e6f, {0x80, 0x00, 0xba, 0x59, 0x89, 0x61, 0x41, 0x4d}};
static const IID kIid2DBuffer = {0x7dc9d5f9, 0x9ed9, 0x44ec, {0x9b, 0xbf, 0x06, 0x00, 0xbb, 0x58, 0x9f, 0xbb}};
static const GUID kCodecApiLowLatency = {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};

enum {
    NIN = 4, INCAP = 4 << 20,              /* input buffers: 4 x 4 MiB, as nxnative */
    NOUT_VIDEO = 16, NOUT_AAC = 32, NOUT_MAX = 32,
    ERR_NO_IMPL = 0x1000, ERR_ARG = 0x100b, ERR_NO_OUTPUT = 0x100e, ERR_NO_INPUT = 0x1011,
    TYPE_AVC = 1, TYPE_AAC = 4, FMT_PCM16 = 1, FMT_NV12 = 4, MODE_CPU = 1, FLAG_EOS = 4,
    EV_INPUT = 1, EV_OUTPUT = 2, EV_FORMAT = 4, FIFO = 64, BACKLOG = 4, POOL_MAX = 96,
};

typedef struct Frame { uint8_t *data; size_t bytes; int64_t pts; uint32_t flags; int pic; } Frame;
typedef struct Decoder {
    SRWLOCK mu;                 /* queues, slots and frame buffer pool */
    SRWLOCK dmu;                /* one packet through Media Foundation at a time, in arrival order */
    int type, fmt, mode, nout;
    uint64_t events;            /* the client's DecoderEvents (4 EventType pointers), read when signalled */
    int started, in_eos, fmt_sent, w, h;
    uint8_t *in[NIN]; int infree[NIN], ninfree;
    struct { int in, off, size; int64_t pts; uint32_t flags; } pend[NIN]; int npend;
    Frame fifo[FIFO]; int nfifo;                              /* decoded, waiting for a free output slot */
    struct { int used, cli; Frame f; } out[NOUT_MAX];
    int outq[NOUT_MAX], noutq;
    uint8_t *pool[POOL_MAX]; size_t pool_cap[POOL_MAX]; int npool;
    /* Media Foundation; touched only under dmu */
    IMFTransform *mft; int out_w, out_h, out_stride, out_plane_h;
    uint8_t *hdr; size_t nhdr; int nal_len, need_hdr;          /* H.264 SPS/PPS as Annex B, prepended after a (re)start */
    uint8_t asc[64]; int nasc, ch, rate;                       /* AAC AudioSpecificConfig, channels, rate */
    uint8_t *annexb; size_t annexb_cap;
    uint64_t packets, frames; unsigned errors;
    Frame previous; /* private copy for optional midframes, protected by dmu */
    /* DKR_PERF=1: per second, outputs the client took, decode time, and whether audio carried sound */
    LARGE_INTEGER stat_start; int64_t decode_ticks, decode_worst; unsigned stat_packets, stat_taken, stat_loud;
} Decoder;

static uint8_t *alloc_frame_buf(Decoder *d, size_t bytes) {
    if (!bytes) return NULL;
    AcquireSRWLockExclusive(&d->mu);
    for (int i = d->npool - 1; i >= 0; i--) {
        if (d->pool_cap[i] == bytes) {
            uint8_t *p = d->pool[i];
            d->npool--;
            d->pool[i] = d->pool[d->npool];
            d->pool_cap[i] = d->pool_cap[d->npool];
            ReleaseSRWLockExclusive(&d->mu);
            return p;
        }
    }
    ReleaseSRWLockExclusive(&d->mu);
    return malloc(bytes);
}

static void free_frame_buf_locked(Decoder *d, uint8_t *p, size_t bytes) {
    if (!p) return;
    if (bytes && d->npool < POOL_MAX) {
        d->pool[d->npool] = p;
        d->pool_cap[d->npool] = bytes;
        d->npool++;
    } else {
        free(p);
    }
}

static void free_frame_buf(Decoder *d, uint8_t *p, size_t bytes) {
    if (!p) return;
    AcquireSRWLockExclusive(&d->mu);
    free_frame_buf_locked(d, p, bytes);
    ReleaseSRWLockExclusive(&d->mu);
}

static int perf_on(void) {
    static int on = -1;
    if (on < 0) { const char *v = getenv("DKR_PERF"); on = v && *v && strcmp(v, "0"); }
    return on;
}
static void stats(Decoder *d) {
    if (!perf_on()) return;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
    if (!d->stat_start.QuadPart) d->stat_start = now;
    if (now.QuadPart - d->stat_start.QuadPart < freq.QuadPart) return;
    double ms = 1000.0 / (double)freq.QuadPart;
    hos_log("perf movie: %s %u outputs/s taken, %u packets decoded (avg %.2f ms, worst %.2f ms)%s\n",
            d->type == TYPE_AVC ? "video" : "audio", d->stat_taken, d->stat_packets,
            d->stat_packets ? d->decode_ticks * ms / d->stat_packets : 0.0, d->decode_worst * ms,
            d->type == TYPE_AAC ? (d->stat_loud ? ", with sound" : ", silent") : "");
    d->stat_start = now; d->decode_ticks = d->decode_worst = 0; d->stat_packets = d->stat_taken = d->stat_loud = 0;
}

/* ---------- guest helpers ---------- */
static uint64_t gcall(Cpu *c, uint64_t address, unsigned n, const uint64_t *args) {
    Cpu saved = *c;
    for (unsigned i = 0; i < n; i++) c->x[i] = args[i];
    rt_call(c, address);
    uint64_t result = c->x[0];
    *c = saved;
    return result;
}
#define G(c, fn, ...) gcall((c), dkr_movie_sdk.fn, sizeof((uint64_t[]){__VA_ARGS__}) / 8, (uint64_t[]){__VA_ARGS__})
#define A(p) ((uint64_t)(uintptr_t)(p))
static Decoder *of(Cpu *c) { return c->x[0] ? (Decoder *)(uintptr_t)ld64(c->x[0] + 8) : NULL; }
static void signal_events(Cpu *c, Decoder *d, unsigned mask) {
    for (int i = 0; i < 4; i++) if (mask & (1u << i)) {
        uint64_t event = d->events ? ld64(d->events + 8u * i) : 0;
        if (event && !(event & 7)) G(c, signal_event, event);
    }
}
static void set_i32(Cpu *c, uint64_t md, const char *key, int v) { G(c, media_set_int32, md, A(key), (uint64_t)(uint32_t)v); }
static int find_i32(Cpu *c, uint64_t md, const char *key, int *v) { return (int)(G(c, media_find_int32, md, A(key), A(v)) & 1); }
static int find_buffer(Cpu *c, uint64_t md, const char *key, const uint8_t **data, size_t *size) {
    uint64_t buffer = 0;
    if (!(G(c, media_find_buffer, md, A(key), A(&buffer)) & 1) || !buffer) return 0;
    *data = (const uint8_t *)(uintptr_t)G(c, buffer_data, buffer);
    *size = (size_t)G(c, buffer_size, buffer);
    return *data && *size;
}

/* ---------- H.264 bitstream: Annex B for Media Foundation ---------- */
static int annexb_start(const uint8_t *p, size_t n) { return (n >= 4 && !p[0] && !p[1] && !p[2] && p[3] == 1) || (n >= 3 && !p[0] && !p[1] && p[2] == 1); }
static void append(uint8_t **buf, size_t *len, size_t *cap, const void *p, size_t n) {
    if (*len + n > *cap) {
        size_t want = (*len + n) * 2;
        uint8_t *grown = realloc(*buf, want);
        if (!grown) return;
        *buf = grown; *cap = want;
    }
    memcpy(*buf + *len, p, n); *len += n;
}
static void header_nal(Decoder *d, const uint8_t *nal, size_t n) {
    static const uint8_t code[] = {0, 0, 0, 1};
    size_t cap = d->nhdr;
    append(&d->hdr, &d->nhdr, &cap, code, 4);
    cap = d->nhdr;
    append(&d->hdr, &d->nhdr, &cap, nal, n);
}
/* csd-0 / csd-1 (Annex B, as MediaCodec takes them) or an avcC record (AVCDecoderConfigurationRecord) */
static void keep_parameter_sets(Decoder *d, const uint8_t *p, size_t n) {
    if (annexb_start(p, n)) {
        size_t cap = d->nhdr;
        append(&d->hdr, &d->nhdr, &cap, p, n);
    } else if (n >= 7 && p[0] == 1) {
        d->nal_len = (p[4] & 3) + 1;
        size_t i = 5;
        for (int set = 0; set < 2 && i < n; set++) {
            int count = set ? p[i] : p[i] & 31; i++;
            for (int k = 0; k < count && i + 2 <= n; k++) {
                size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2;
                if (len > n - i) return;
                header_nal(d, p + i, len); i += len;
            }
        }
    }
}
/* One access unit as Annex B: start codes kept, or length prefixes (avcC NAL length size) rewritten. */
static size_t to_annexb(Decoder *d, const uint8_t *p, size_t n) {
    size_t len = 0, cap = d->annexb_cap;
    if (d->need_hdr && d->nhdr) append(&d->annexb, &len, &cap, d->hdr, d->nhdr);
    if (annexb_start(p, n)) append(&d->annexb, &len, &cap, p, n);
    else {
        static const uint8_t code[] = {0, 0, 0, 1};
        int size = d->nal_len ? d->nal_len : 4;
        for (size_t i = 0; i + (size_t)size <= n;) {
            size_t nal = 0;
            for (int k = 0; k < size; k++) nal = nal << 8 | p[i + k];
            i += (size_t)size;
            if (!nal || nal > n - i) break;
            append(&d->annexb, &len, &cap, code, 4);
            append(&d->annexb, &len, &cap, p + i, nal);
            i += nal;
        }
    }
    d->annexb_cap = cap;
    d->need_hdr = 0;
    return len;
}

/* ---------- Media Foundation ---------- */
static INIT_ONCE g_mf_once = INIT_ONCE_STATIC_INIT;
static HRESULT g_mf_status = E_FAIL;
static BOOL CALLBACK mf_start(PINIT_ONCE once, void *p, void **ctx) {
    (void)once; (void)p; (void)ctx;
    g_mf_status = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    return TRUE;
}
static int mf_ready(void) {
    static _Thread_local int com;
    if (!com) { HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED); com = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE ? 1 : -1; }
    InitOnceExecuteOnce(&g_mf_once, mf_start, NULL, NULL);
    return com > 0 && SUCCEEDED(g_mf_status);
}
static void mf_release(Decoder *d) { if (d->mft) IMFTransform_Release(d->mft); d->mft = NULL; }

static int select_output(Decoder *d) {
    for (DWORD i = 0;; i++) {
        IMFMediaType *type = NULL;
        if (FAILED(IMFTransform_GetOutputAvailableType(d->mft, 0, i, &type))) return 0;
        GUID subtype; UINT32 bits = 0;
        int ok = SUCCEEDED(IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype));
        if (ok && d->type == TYPE_AVC) ok = IsEqualGUID(&subtype, &MFVideoFormat_NV12);
        else if (ok) ok = IsEqualGUID(&subtype, &MFAudioFormat_PCM) &&
                          SUCCEEDED(IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) && bits == 16;
        if (ok && SUCCEEDED(IMFTransform_SetOutputType(d->mft, 0, type, 0))) {
            if (d->type == TYPE_AVC) {
                UINT64 size = 0; UINT32 stride = 0; MFVideoArea area;
                IMFMediaType_GetUINT64(type, &MF_MT_FRAME_SIZE, &size);
                d->out_w = (int)(size >> 32); d->out_plane_h = d->out_h = (int)(uint32_t)size;
                if (SUCCEEDED(IMFMediaType_GetBlob(type, &MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof area, NULL)) &&
                    area.Area.cx > 0 && area.Area.cy > 0) { d->out_w = area.Area.cx; d->out_h = area.Area.cy; }
                d->out_stride = SUCCEEDED(IMFMediaType_GetUINT32(type, &MF_MT_DEFAULT_STRIDE, &stride)) && (int)stride > 0
                                ? (int)stride : (int)(size >> 32);
                hos_log("movie: H.264 output NV12 %dx%d (%d rows allocated, stride %d)\n", d->out_w, d->out_h, d->out_plane_h, d->out_stride);
            } else {
                UINT32 ch = 0, rate = 0;
                IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
                IMFMediaType_GetUINT32(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                if (ch) d->ch = (int)ch;
                if (rate) d->rate = (int)rate;
                hos_log("movie: AAC output PCM16 %u channels %u Hz\n", ch, rate);
            }
            IMFMediaType_Release(type);
            return 1;
        }
        IMFMediaType_Release(type);
    }
}

static int mf_open(Decoder *d) {
    if (d->mft) return 1;
    if (!mf_ready()) { if (!d->errors++) hos_log("movie: Media Foundation is unavailable\n"); return 0; }
    const char *name = d->type == TYPE_AVC ? "H.264" : "AAC";
    HRESULT hr = CoCreateInstance(d->type == TYPE_AVC ? &kClsidH264Decoder : &kClsidAacDecoder, NULL, CLSCTX_INPROC_SERVER,
                                  &kIidTransform, (void **)&d->mft);
    if (FAILED(hr)) { if (!d->errors++) hos_log("movie: cannot create the %s decoder (%#lx)\n", name, hr); d->mft = NULL; return 0; }
    if (d->type == TYPE_AVC) {
        IMFAttributes *attr = NULL;
        if (SUCCEEDED(IMFTransform_GetAttributes(d->mft, &attr)) && attr) {
            IMFAttributes_SetUINT32(attr, &kCodecApiLowLatency, 1);
            IMFAttributes_Release(attr);
        }
    }
    IMFMediaType *in = NULL;
    if (FAILED(MFCreateMediaType(&in))) { mf_release(d); return 0; }
    if (d->type == TYPE_AVC) {
        IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
        if (d->w > 0 && d->h > 0) IMFMediaType_SetUINT64(in, &MF_MT_FRAME_SIZE, (UINT64)d->w << 32 | (UINT32)d->h);
    } else {
        static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
        if (d->nasc >= 2) {
            int index = (d->asc[0] & 7) << 1 | d->asc[1] >> 7, config = d->asc[1] >> 3 & 15;
            if (index < 13 && d->rate <= 0) d->rate = rates[index];
            if (config && config < 8 && d->ch <= 0) d->ch = config == 7 ? 8 : config;
        }
        if (d->rate <= 0) d->rate = 48000;
        if (d->ch <= 0) d->ch = 2;
        if (d->nasc < 2) { /* AAC-LC from the rate and channels */
            int index = 3;
            for (int i = 0; i < 13; i++) if (rates[i] == d->rate) index = i;
            d->asc[0] = (uint8_t)(2 << 3 | index >> 1); d->asc[1] = (uint8_t)((index & 1) << 7 | (d->ch & 15) << 3); d->nasc = 2;
        }
        uint8_t user[12 + sizeof d->asc] = {0}; /* HEAACWAVEINFO tail: raw payload (0), profile unknown (0xfe), then the ASC */
        user[2] = 0xfe;
        memcpy(user + 12, d->asc, (size_t)d->nasc);
        IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFAudioFormat_AAC);
        IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)d->ch);
        IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)d->rate);
        IMFMediaType_SetUINT32(in, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        IMFMediaType_SetUINT32(in, &MF_MT_AAC_PAYLOAD_TYPE, 0);
        IMFMediaType_SetBlob(in, &MF_MT_USER_DATA, user, 12u + (UINT32)d->nasc);
    }
    hr = IMFTransform_SetInputType(d->mft, 0, in, 0);
    IMFMediaType_Release(in);
    if (FAILED(hr) || !select_output(d)) {
        if (!d->errors++) hos_log("movie: the %s decoder rejected the stream types (%#lx)\n", name, hr);
        mf_release(d);
        return 0;
    }
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (d->type == TYPE_AAC) hos_log("movie: AAC %d channels %d Hz, AudioSpecificConfig %d bytes\n", d->ch, d->rate, d->nasc);
    return 1;
}

static void push_frame(Decoder *d, Frame f) {
    AcquireSRWLockExclusive(&d->mu);
    if (d->nfifo < FIFO) d->fifo[d->nfifo++] = f;
    else free_frame_buf_locked(d, f.data, f.bytes);
    ReleaseSRWLockExclusive(&d->mu);
}

/* A decoded Media Foundation sample → a Frame in the FIFO (the caller holds dmu). */
static void take_sample(Decoder *d, IMFSample *sample, int64_t pkt_pts) {
    LONGLONG time = 0;
    HRESULT hr_time = IMFSample_GetSampleTime(sample, &time);
    int64_t pts = SUCCEEDED(hr_time) ? time / 10 : (pkt_pts >= 0 ? pkt_pts : 0);
    if (d->type == TYPE_AAC && pkt_pts >= 0) pts = pkt_pts;
    Frame f = {.pts = pts, .pic = 1};
    if (d->type == TYPE_AVC) {
        int w = d->w > 0 ? d->w : d->out_w, h = d->h > 0 ? d->h : d->out_h;
        f.bytes = (size_t)w * h * 3 / 2;
        if (!(f.data = alloc_frame_buf(d, f.bytes))) return;
        IMFMediaBuffer *buffer = NULL; IMF2DBuffer *two = NULL;
        BYTE *scan0 = NULL; LONG pitch = 0; int locked2d = 0, locked = 0;
        if (SUCCEEDED(IMFSample_GetBufferByIndex(sample, 0, &buffer))) {
            if (SUCCEEDED(IMFMediaBuffer_QueryInterface(buffer, &kIid2DBuffer, (void **)&two)) && SUCCEEDED(IMF2DBuffer_Lock2D(two, &scan0, &pitch)))
                locked2d = 1;
            else if (SUCCEEDED(IMFMediaBuffer_Lock(buffer, &scan0, NULL, NULL))) { locked = 1; pitch = d->out_stride; }
        }
        int cw = w < d->out_w ? w : d->out_w, chh = h < d->out_h ? h : d->out_h;
        if (!scan0 || pitch <= 0 || cw < w || chh < h) { memset(f.data, 16, (size_t)w * h); memset(f.data + (size_t)w * h, 128, (size_t)w * h / 2); }
        if (scan0 && pitch > 0) {
            if (pitch == w && cw == w) {
                memcpy(f.data, scan0, (size_t)w * chh);
            } else {
                for (int y = 0; y < chh; y++) memcpy(f.data + (size_t)y * w, scan0 + (size_t)y * pitch, (size_t)cw);
            }
            const BYTE *uv = scan0 + (size_t)pitch * d->out_plane_h;
            int uvh = (chh + 1) / 2 < h / 2 ? (chh + 1) / 2 : h / 2;
            if (pitch == w && (cw & ~1) == w) {
                memcpy(f.data + (size_t)w * h, uv, (size_t)w * uvh);
            } else {
                for (int y = 0; y < uvh; y++) memcpy(f.data + (size_t)w * h + (size_t)y * w, uv + (size_t)y * pitch, (size_t)(cw & ~1));
            }
        }
        if (locked2d) IMF2DBuffer_Unlock2D(two);
        if (locked) IMFMediaBuffer_Unlock(buffer);
        if (two) IMF2DBuffer_Release(two);
        if (buffer) IMFMediaBuffer_Release(buffer);
    } else {
        IMFMediaBuffer *buffer = NULL; BYTE *p = NULL; DWORD n = 0;
        if (FAILED(IMFSample_ConvertToContiguousBuffer(sample, &buffer))) return;
        if (SUCCEEDED(IMFMediaBuffer_Lock(buffer, &p, NULL, &n))) {
            if (n && (f.data = alloc_frame_buf(d, n))) { memcpy(f.data, p, n); f.bytes = n; }
            IMFMediaBuffer_Unlock(buffer);
        }
        IMFMediaBuffer_Release(buffer);
        if (!f.data) return;
    }
    if (d->type == TYPE_AVC && dkr_settings()->movie_smoothing) {
        int64_t midpoint = dkr_movie_midpoint(d->previous.pts, f.pts);
        if (d->previous.data && d->previous.bytes == f.bytes && midpoint >= 0) {
            Frame middle = {.bytes = f.bytes, .pts = midpoint, .pic = 1};
            middle.data = alloc_frame_buf(d, middle.bytes);
            if (middle.data) {
                dkr_movie_midframe(middle.data, d->previous.data, f.data, f.bytes, f.bytes * 2 / 3);
                push_frame(d, middle);
            }
        }
        if (!d->previous.data || d->previous.bytes != f.bytes) {
            free_frame_buf(d, d->previous.data, d->previous.bytes);
            d->previous.data = alloc_frame_buf(d, f.bytes);
        }
        d->previous.bytes = f.bytes; d->previous.pts = f.pts;
        if (d->previous.data) memcpy(d->previous.data, f.data, f.bytes);
    }
    d->frames++;
    push_frame(d, f);
}

/* Collect every output Media Foundation has ready (the caller holds dmu). */
static void drain_outputs(Decoder *d, int64_t pkt_pts) {
    for (int guard = 0; guard < 64; guard++) {
        MFT_OUTPUT_STREAM_INFO info = {0};
        if (FAILED(IMFTransform_GetOutputStreamInfo(d->mft, 0, &info))) return;
        MFT_OUTPUT_DATA_BUFFER out = {0};
        IMFSample *own = NULL;
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            IMFMediaBuffer *buffer = NULL;
            if (FAILED(MFCreateSample(&own)) || FAILED(MFCreateMemoryBuffer(info.cbSize ? info.cbSize : 1 << 20, &buffer))) {
                if (own) IMFSample_Release(own);
                return;
            }
            IMFSample_AddBuffer(own, buffer);
            IMFMediaBuffer_Release(buffer);
            out.pSample = own;
        }
        DWORD status = 0;
        HRESULT hr = IMFTransform_ProcessOutput(d->mft, 0, 1, &out, &status);
        if (out.pEvents) IMFCollection_Release(out.pEvents);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (own) IMFSample_Release(own);
            if (!select_output(d)) { if (!d->errors++) hos_log("movie: no NV12/PCM16 output after a format change\n"); return; }
            continue;
        }
        if (SUCCEEDED(hr) && out.pSample) {
            take_sample(d, out.pSample, pkt_pts);
            pkt_pts = -1;
        }
        if (out.pSample && out.pSample != own) IMFSample_Release(out.pSample);
        if (own) IMFSample_Release(own);
        if (FAILED(hr)) {
            if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT && !d->errors++) hos_log("movie: ProcessOutput failed (%#lx)\n", hr);
            return;
        }
    }
}

/* One packet through Media Foundation (the caller holds dmu). */
static void decode_packet(Decoder *d, const uint8_t *p, size_t n, int64_t pts, int eos) {
    if (!mf_open(d)) return;
    if (n) {
        size_t len = n;
        if (d->type == TYPE_AVC) { len = to_annexb(d, p, n); p = d->annexb; }
        IMFMediaBuffer *buffer = NULL; IMFSample *sample = NULL; BYTE *dst = NULL;
        if (len && SUCCEEDED(MFCreateMemoryBuffer((DWORD)len, &buffer)) && SUCCEEDED(IMFMediaBuffer_Lock(buffer, &dst, NULL, NULL))) {
            memcpy(dst, p, len);
            IMFMediaBuffer_Unlock(buffer);
            IMFMediaBuffer_SetCurrentLength(buffer, (DWORD)len);
            if (SUCCEEDED(MFCreateSample(&sample))) {
                IMFSample_AddBuffer(sample, buffer);
                IMFSample_SetSampleTime(sample, pts * 10);
                if (d->type == TYPE_AAC) {
                    LONGLONG dur = d->rate > 0 ? (LONGLONG)1024 * 10000000 / d->rate : 213333;
                    IMFSample_SetSampleDuration(sample, dur);
                }
                HRESULT hr = IMFTransform_ProcessInput(d->mft, 0, sample, 0);
                if (hr == MF_E_NOTACCEPTING) { drain_outputs(d, -1); hr = IMFTransform_ProcessInput(d->mft, 0, sample, 0); }
                if (FAILED(hr) && !d->errors++) hos_log("movie: ProcessInput failed (%#lx)\n", hr);
                IMFSample_Release(sample);
            }
        }
        if (buffer) IMFMediaBuffer_Release(buffer);
        d->packets++;
    }
    drain_outputs(d, pts);
    if (eos) {
        IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_COMMAND_DRAIN, 0);
        drain_outputs(d, -1);
        IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    }
}

/* ---------- the MediaCodec-style queues ---------- */
static void reset_queues(Decoder *d) {
    AcquireSRWLockExclusive(&d->mu);
    for (int i = 0; i < NIN; i++) d->infree[i] = i;
    d->ninfree = NIN; d->npend = 0; d->in_eos = 0; d->noutq = 0;
    for (int i = 0; i < d->nfifo; i++) free_frame_buf_locked(d, d->fifo[i].data, d->fifo[i].bytes);
    d->nfifo = 0;
    for (int i = 0; i < NOUT_MAX; i++) {
        free_frame_buf_locked(d, d->out[i].f.data, d->out[i].f.bytes);
        memset(&d->out[i], 0, sizeof d->out[i]);
    }
    ReleaseSRWLockExclusive(&d->mu);
}
static void flush_decoder(Decoder *d) {
    AcquireSRWLockExclusive(&d->dmu);
    free_frame_buf(d, d->previous.data, d->previous.bytes);
    memset(&d->previous, 0, sizeof d->previous);
    if (d->mft) IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_COMMAND_FLUSH, 0);
    d->need_hdr = 1;
    ReleaseSRWLockExclusive(&d->dmu);
    reset_queues(d);
}

/* Decode queued packets while the decoded backlog is small, hand decoded frames to free output slots, then signal. */
static void pump(Cpu *c, Decoder *d) {
    unsigned signals = 0;
    AcquireSRWLockExclusive(&d->dmu);
    for (;;) {
        AcquireSRWLockExclusive(&d->mu);
        for (int s = 0; s < d->nout && d->nfifo; s++) if (!d->out[s].used) {
            d->out[s].used = 1; d->out[s].cli = 0; d->out[s].f = d->fifo[0];
            memmove(d->fifo, d->fifo + 1, (size_t)--d->nfifo * sizeof *d->fifo);
            d->outq[d->noutq++] = s;
            signals |= EV_OUTPUT;
        }
        if (d->noutq > 0 && d->started) signals |= EV_OUTPUT;
        if (!d->npend || d->nfifo >= BACKLOG) { ReleaseSRWLockExclusive(&d->mu); break; }
        __typeof__(d->pend[0]) pk = d->pend[0];
        memmove(d->pend, d->pend + 1, (size_t)--d->npend * sizeof *d->pend);
        ReleaseSRWLockExclusive(&d->mu);
        int eos = (pk.flags & FLAG_EOS) != 0;
        if (eos && (pk.pts < 0 || pk.size >= INCAP)) pk.size = 0; /* an end-of-stream packet without data (nxnative) */
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        decode_packet(d, d->in[pk.in] + pk.off, (size_t)pk.size, pk.pts, eos);
        QueryPerformanceCounter(&t1);
        d->decode_ticks += t1.QuadPart - t0.QuadPart; d->stat_packets++;
        if (t1.QuadPart - t0.QuadPart > d->decode_worst) d->decode_worst = t1.QuadPart - t0.QuadPart;
        AcquireSRWLockExclusive(&d->mu);
        d->infree[d->ninfree++] = pk.in;
        if (!d->in_eos) signals |= EV_INPUT;
        if (eos && d->nfifo < FIFO) d->fifo[d->nfifo++] = (Frame){.pts = pk.pts, .flags = FLAG_EOS}; /* carries the flag back */
        ReleaseSRWLockExclusive(&d->mu);
    }
    ReleaseSRWLockExclusive(&d->dmu);
    signal_events(c, d, signals);
}

/* ---------- movie::Decoder members ---------- */
static int dims(const Decoder *d, int *w, int *h) {
    *w = d->w > 0 ? d->w : d->out_w > 0 ? d->out_w : 1920;
    *h = d->h > 0 ? d->h : d->out_h > 0 ? d->out_h : 1080;
    return *w * *h * 3 / 2;
}

/* Decoder(DecoderType, DecoderOutputFormat, DecoderMode[, priority]) */
void dkr_movie_ctor(Cpu *c) {
    uint64_t self = c->x[0];
    int type = (int)c->x[1], fmt = (int)c->x[2], mode = (int)c->x[3];
    st64(self, dkr_movie_sdk.vtable + 0x10);
    st64(self + 8, 0);
    int audio = type == TYPE_AAC && fmt == FMT_PCM16, video = type == TYPE_AVC && fmt == FMT_NV12 && mode == MODE_CPU;
    if (!audio && !video) {
        hos_log("movie: decoder type %d, output %d, mode %d is not supported; its calls fail\n", type, fmt, mode);
        return;
    }
    Decoder *d = calloc(1, sizeof *d);
    if (!d) hos_stop(c, "movie: out of memory creating a decoder");
    InitializeSRWLock(&d->mu); InitializeSRWLock(&d->dmu);
    d->type = type; d->fmt = fmt; d->mode = mode; d->nout = audio ? NOUT_AAC : NOUT_VIDEO; d->need_hdr = 1;
    for (int i = 0; i < NIN; i++) {
        if (!(d->in[i] = malloc(INCAP))) hos_stop(c, "movie: out of memory for decoder input buffers");
        d->infree[i] = i;
    }
    d->ninfree = NIN;
    st64(self + 8, (uint64_t)(uintptr_t)d);
    hos_log("movie: decoder %#llx: %s (type %d, output %d, mode %d) on Media Foundation\n", (unsigned long long)self,
            audio ? "AAC" : "H.264", type, fmt, mode);
}

static void destroy(Cpu *c) {
    Decoder *d = of(c);
    if (!d) return;
    st64(c->x[0] + 8, 0);
    hos_log("movie: decoder %#llx destroyed: %llu packets, %llu frames decoded, %u errors\n", (unsigned long long)c->x[0],
            (unsigned long long)d->packets, (unsigned long long)d->frames, d->errors);
    AcquireSRWLockExclusive(&d->dmu); mf_release(d); free(d->previous.data); d->previous.data = NULL; ReleaseSRWLockExclusive(&d->dmu);
    reset_queues(d);
    for (int i = 0; i < d->npool; i++) free(d->pool[i]);
    for (int i = 0; i < NIN; i++) free(d->in[i]);
    free(d->hdr); free(d->annexb); free(d);
}
void dkr_movie_dtor(Cpu *c) { destroy(c); }
void dkr_movie_dtor_delete(Cpu *c) {
    /* The SDK's deleting destructor runs the base destructor (replaced above, now a no-op) and the game's operator delete. */
    destroy(c);
    rt_call_original(c, "dkr_movie_dtor_delete");
}

void dkr_movie_configure(Cpu *c) { /* Configure(MediaData *) */
    Decoder *d = of(c); uint64_t md = c->x[1];
    if (!d) { c->x[0] = ERR_NO_IMPL; return; }
    const uint8_t *data; size_t size; int w = 0, h = 0, ch = 0, rate = 0;
    if (md && d->type == TYPE_AVC) {
        AcquireSRWLockExclusive(&d->dmu);
        free(d->hdr); d->hdr = NULL; d->nhdr = 0;
        ReleaseSRWLockExclusive(&d->dmu);
        const char *keys[] = {"csd-0", "csd-1", "avcc"};
        for (int k = 0; k < 3; k++) if (find_buffer(c, md, keys[k], &data, &size)) {
            AcquireSRWLockExclusive(&d->dmu); keep_parameter_sets(d, data, size); ReleaseSRWLockExclusive(&d->dmu);
        }
        find_i32(c, md, "width", &w); find_i32(c, md, "height", &h);
        if (w > 0 && h > 0 && !d->fmt_sent) { d->w = w; d->h = h; }
    } else if (md) {
        if (find_buffer(c, md, "csd-0", &data, &size) && size <= sizeof d->asc) { memcpy(d->asc, data, size); d->nasc = (int)size; }
        if (find_i32(c, md, "channel-count", &ch) && ch > 0) d->ch = ch;
        if (find_i32(c, md, "sample-rate", &rate) && rate > 0) d->rate = rate;
    }
    hos_log("movie: Configure %s: %dx%d, %d channels %d Hz, parameter sets %zu bytes, ASC %d bytes\n",
            d->type == TYPE_AVC ? "H.264" : "AAC", w, h, ch, rate, d->nhdr, d->nasc);
    c->x[0] = 0;
}

void dkr_movie_register_events(Cpu *c) { Decoder *d = of(c); if (d) d->events = c->x[1]; c->x[0] = d ? 0 : ERR_NO_IMPL; }
void dkr_movie_start(Cpu *c) {
    Decoder *d = of(c);
    if (!d) { c->x[0] = 0x2002; return; }
    AcquireSRWLockExclusive(&d->mu); d->started = 1; ReleaseSRWLockExclusive(&d->mu);
    signal_events(c, d, EV_INPUT);
    c->x[0] = 0;
}
void dkr_movie_stop(Cpu *c) {
    Decoder *d = of(c);
    if (!d) { c->x[0] = 0x2003; return; }
    AcquireSRWLockExclusive(&d->mu); d->started = 0; ReleaseSRWLockExclusive(&d->mu);
    flush_decoder(d);
    c->x[0] = 0;
}
void dkr_movie_flush(Cpu *c) { Decoder *d = of(c); if (d) flush_decoder(d); c->x[0] = d ? 0 : ERR_NO_IMPL; }
void dkr_movie_reset(Cpu *c) {
    Decoder *d = of(c);
    if (!d) { c->x[0] = ERR_NO_IMPL; return; }
    flush_decoder(d);
    AcquireSRWLockExclusive(&d->dmu); mf_release(d); d->fmt_sent = 0; ReleaseSRWLockExclusive(&d->dmu);
    c->x[0] = 0;
}
void dkr_movie_last_error(Cpu *c) { c->x[0] = 0; }

void dkr_movie_acquire_input(Cpu *c) { /* AcquireInputBufferIndex(int *index, int *remaining) */
    Decoder *d = of(c); uint64_t index = c->x[1], left = c->x[2];
    if (!d) { c->x[0] = ERR_NO_IMPL; return; }
    if (!index || !left) { c->x[0] = ERR_ARG; return; }
    AcquireSRWLockExclusive(&d->mu);
    int result = ERR_NO_INPUT, i = -1;
    if (d->started && !d->in_eos && d->ninfree) { i = d->infree[--d->ninfree]; result = 0; }
    st32(index, (uint32_t)i); st32(left, (uint32_t)d->ninfree);
    int more_in = d->started && !d->in_eos && d->ninfree > 0;
    ReleaseSRWLockExclusive(&d->mu);
    if (result == 0 && more_in) signal_events(c, d, EV_INPUT);
    c->x[0] = (uint64_t)result;
}
void dkr_movie_get_input(Cpu *c) { /* GetInputBuffer(int index, Buffer *) */
    Decoder *d = of(c); int i = (int)c->x[1]; uint64_t buffer = c->x[2];
    if (!d) { c->x[0] = ERR_NO_IMPL; return; }
    if (!buffer || i < 0 || i >= NIN) { c->x[0] = ERR_ARG; return; }
    G(c, buffer_set_data, buffer, A(d->in[i]), INCAP);
    G(c, buffer_set_range, buffer, 0, INCAP);
    c->x[0] = 0;
}
void dkr_movie_send_input(Cpu *c) { /* SendInputBufferForDecode(int index, int offset, int size, long time_us, uint flags) */
    Decoder *d = of(c);
    int i = (int)c->x[1], off = (int)c->x[2], size = (int)c->x[3];
    int64_t pts = (int64_t)c->x[4]; uint32_t flags = (uint32_t)c->x[5];
    if (!d) { c->x[0] = ERR_NO_IMPL; return; }
    if (i < 0 || i >= NIN || off < 0 || size < 0 || (uint64_t)off + (uint64_t)size > INCAP) { c->x[0] = ERR_ARG; return; }
    AcquireSRWLockExclusive(&d->mu);
    for (int k = 0; k < d->ninfree; k++) if (d->infree[k] == i) { /* sent after a flush returned it: claim it back */
        memmove(d->infree + k, d->infree + k + 1, (size_t)(--d->ninfree - k) * sizeof *d->infree);
        break;
    }
    if (d->npend == NIN) { ReleaseSRWLockExclusive(&d->mu); c->x[0] = ERR_ARG; return; }
    if (flags & FLAG_EOS) d->in_eos = 1;
    d->pend[d->npend].in = i; d->pend[d->npend].off = off; d->pend[d->npend].size = size;
    d->pend[d->npend].pts = pts; d->pend[d->npend].flags = flags; d->npend++;
    int first = !d->fmt_sent;
    d->fmt_sent = 1;
    ReleaseSRWLockExclusive(&d->mu);
    if (first) signal_events(c, d, EV_FORMAT);
    pump(c, d);
    c->x[0] = 0;
}

void dkr_movie_acquire_output(Cpu *c) { /* AcquireOutputBufferIndex(int *index, long *time, uint *flags, int *remaining) */
    Decoder *d = of(c); uint64_t index = c->x[1], pts = c->x[2], flags = c->x[3], left = c->x[4];
    if (!d) { c->x[0] = ERR_NO_IMPL; return; }
    if (!index || !pts || !flags || !left) { c->x[0] = ERR_ARG; return; }
    AcquireSRWLockExclusive(&d->mu);
    int result = ERR_NO_OUTPUT, s = -1;
    if (d->noutq && d->started) {
        s = d->outq[0];
        memmove(d->outq, d->outq + 1, (size_t)--d->noutq * sizeof *d->outq);
        d->out[s].cli = 1;
        st64(pts, (uint64_t)d->out[s].f.pts); st32(flags, d->out[s].f.flags);
        result = 0;
        d->stat_taken++;
        if (d->type == TYPE_AAC && d->out[s].f.data)
            for (size_t k = 0; k + 1 < d->out[s].f.bytes && !d->stat_loud; k += 2) d->stat_loud = d->out[s].f.data[k] || d->out[s].f.data[k + 1];
    }
    st32(index, (uint32_t)s); st32(left, (uint32_t)d->noutq);
    int more_out = d->noutq > 0 && d->started;
    ReleaseSRWLockExclusive(&d->mu);
    if (result == 0 && more_out) signal_events(c, d, EV_OUTPUT);
    stats(d);
    c->x[0] = (uint64_t)result;
}

void dkr_movie_output_size(Cpu *c) { /* GetOutputBufferSize(int *size, int *mode) */
    Decoder *d = of(c); uint64_t size = c->x[1], mode = c->x[2];
    if (!d) { c->x[0] = ERR_NO_IMPL; return; }
    if (!size || !mode) { c->x[0] = ERR_ARG; return; }
    int w, h, bytes = dims(d, &w, &h);
    if (d->type == TYPE_AAC) {
        int sbr = d->nasc > 0 && ((d->asc[0] >> 3) == 5 || (d->asc[0] >> 3) == 29);
        bytes = 1024 * (sbr ? 2 : 1) * (d->ch > 0 ? d->ch : 2) * 2;
    }
    st32(size, (uint32_t)bytes); st32(mode, (uint32_t)d->mode);
    c->x[0] = 0;
}

static void write_format(Cpu *c, Decoder *d, uint64_t md) {
    if (d->type == TYPE_AAC) {
        set_i32(c, md, "channel-count", d->ch > 0 ? d->ch : 2);
        set_i32(c, md, "sample-rate", d->rate > 0 ? d->rate : 48000);
        return;
    }
    int w, h;
    dims(d, &w, &h);
    set_i32(c, md, "width", w); set_i32(c, md, "height", h);
    G(c, media_set_rect, md, A("crop"), 0, 0, (uint64_t)(uint32_t)(w - 1), (uint64_t)(uint32_t)(h - 1));
    set_i32(c, md, "stride", w); set_i32(c, md, "slice-height", h);
    set_i32(c, md, "nv12-y-stride", w); set_i32(c, md, "nv12-y-offset", 0); set_i32(c, md, "nv12-uv-offset", w * h);
    set_i32(c, md, "nv12-uv-stride", w); set_i32(c, md, "nv12-width", w); set_i32(c, md, "nv12-height", h);
}
void dkr_movie_output_format(Cpu *c) { Decoder *d = of(c); if (d && c->x[1]) write_format(c, d, c->x[1]); c->x[0] = !d ? ERR_NO_IMPL : c->x[1] ? 0 : ERR_ARG; }
void dkr_movie_output_buffer_format(Cpu *c) { Decoder *d = of(c); if (d && c->x[2]) write_format(c, d, c->x[2]); c->x[0] = !d ? ERR_NO_IMPL : c->x[2] ? 0 : ERR_ARG; }

static uint64_t copy_output(Cpu *c, Decoder *d, int i, uint64_t buffer) {
    if (!buffer || i < 0 || i >= d->nout) return ERR_ARG;
    G(c, buffer_set_range, buffer, 0, 0); /* writes start at the buffer's base whatever offset the client left */
    uint8_t *dst = (uint8_t *)(uintptr_t)G(c, buffer_data, buffer);
    size_t capacity = (size_t)G(c, buffer_capacity, buffer);
    AcquireSRWLockShared(&d->mu);
    Frame f = d->out[i].f;
    size_t need = d->type == TYPE_AAC ? (f.pic ? f.bytes : 0) : 0;
    int w = 0, h = 0;
    if (d->type == TYPE_AVC) need = (size_t)dims(d, &w, &h);
    int ok = dst && capacity >= need;
    if (ok && need) {
        if (f.pic && f.data && f.bytes == need) memcpy(dst, f.data, need);
        else if (d->type == TYPE_AVC) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, need - (size_t)w * h); }
        else if (f.data) memcpy(dst, f.data, need);
    }
    ReleaseSRWLockShared(&d->mu);
    if (!ok) {
        hos_log("movie: client output buffer has %zu bytes, the frame needs %zu\n", capacity, need);
        return ERR_ARG;
    }
    G(c, buffer_set_range, buffer, 0, (uint64_t)(uint32_t)need);
    return 0;
}
void dkr_movie_get_output(Cpu *c) { /* GetOutputBuffer(int index, Buffer *) */
    Decoder *d = of(c);
    c->x[0] = d ? copy_output(c, d, (int)c->x[1], c->x[2]) : ERR_NO_IMPL;
}
void dkr_movie_get_output_format(Cpu *c) { /* GetOutputBuffer(int index, Buffer *, MediaData *) */
    Decoder *d = of(c);
    if (!d) { c->x[0] = ERR_NO_IMPL; return; }
    int i = (int)c->x[1]; uint64_t buffer = c->x[2], md = c->x[3];
    if (md) write_format(c, d, md);
    c->x[0] = copy_output(c, d, i, buffer);
}
void dkr_movie_release_output(Cpu *c) { /* ReleaseOutputBufferIndex(int index) */
    Decoder *d = of(c); int i = (int)c->x[1];
    if (!d) { c->x[0] = ERR_NO_IMPL; return; }
    if (i < 0 || i >= d->nout) { c->x[0] = ERR_ARG; return; }
    AcquireSRWLockExclusive(&d->mu);
    int held = d->out[i].used && d->out[i].cli, queued = d->npend || d->nfifo;
    if (held) {
        free_frame_buf_locked(d, d->out[i].f.data, d->out[i].f.bytes);
        memset(&d->out[i], 0, sizeof d->out[i]);
    }
    int more_out = d->noutq > 0 && d->started;
    ReleaseSRWLockExclusive(&d->mu);
    if (held && queued) pump(c, d);
    else if (held && more_out) signal_events(c, d, EV_OUTPUT);
    c->x[0] = 0;
}
/* The constructor overload with a thread priority (unused by the host decoder) */
void dkr_movie_ctor_priority(Cpu *c) { dkr_movie_ctor(c); }
