/* Optional frame statistics (DKR_PERF=1): where the wall time of each presented frame goes.
 * Counters are plain adds; the replay lock serializes almost all writers, so totals are approximate only. */
#ifndef DKR_GFX_PERF_H
#define DKR_GFX_PERF_H
#include <stdint.h>

typedef struct DkrPerf {
    int64_t replay;     /* whole QueueSubmitCommands replay, including the GPU wait */
    int64_t gpu_wait;   /* vkWaitForFences after each replay batch */
    int64_t program;    /* shader variant key + digest + cache lookup, per draw */
    int64_t upload;     /* texture linearize + ASTC decode + staging */
    int64_t present;    /* dkr_nvn_replay_present (blit, acquire, present) */
    int64_t pace;       /* the 60 Hz Sleep in queue_present */
    int64_t submits, draws, temp_buffers, temp_bytes, uploads, pipelines;
} DkrPerf;

extern DkrPerf dkr_perf;
extern int dkr_perf_on;           /* set once by dkr_perf_init */
void dkr_perf_init(void);
int64_t dkr_perf_now(void);       /* QueryPerformanceCounter ticks */
void dkr_perf_frame(void);        /* once per present; prints one line per second */

#define DKR_PERF_ADD(field, value) do { if (dkr_perf_on) dkr_perf.field += (value); } while (0)
#endif
