/* Optional frame statistics, enabled with DKR_PERF=1 (see perf.h). */
#include "perf.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DkrPerf dkr_perf;
int dkr_perf_on;
void dkr_perf_init(void) { const char *v = getenv("DKR_PERF"); dkr_perf_on = v && *v && strcmp(v, "0"); }
int64_t dkr_perf_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
void dkr_perf_frame(void) {
    static int64_t start, last, worst, frames;
    if (!dkr_perf_on) return;
    int64_t now = dkr_perf_now();
    if (!start) { start = last = now; memset(&dkr_perf, 0, sizeof dkr_perf); return; }
    if (now - last > worst) worst = now - last;
    last = now; frames++;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    if (now - start < f.QuadPart) return;
    double ms = 1000.0 / (double)f.QuadPart, n = (double)frames, wall = (double)(now - start) * ms;
    DkrPerf p = dkr_perf;
    fprintf(stderr, "perf: %.1f fps, frame %.2f ms (worst %.2f) | replay %.2f (gpu wait %.2f, shader key %.2f, tex upload %.2f) "
            "present %.2f pace %.2f | per frame: %.1f submits, %.0f draws, %.0f temp buffers, %.0f KiB, %.1f uploads, %lld new pipelines\n",
            n * 1000.0 / wall, wall / n, (double)worst * ms, p.replay * ms / n, p.gpu_wait * ms / n, p.program * ms / n,
            p.upload * ms / n, p.present * ms / n, p.pace * ms / n, p.submits / n, p.draws / n, p.temp_buffers / n,
            p.temp_bytes / 1024.0 / n, p.uploads / n, (long long)p.pipelines);
    fflush(stderr);
    memset(&dkr_perf, 0, sizeof dkr_perf);
    start = now; worst = 0; frames = 0;
}
