/* Optional frame statistics, enabled with DKR_PERF=1 (see perf.h). */
#include "perf.h"
#include "dkr/settings.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DkrPerf dkr_perf;
int dkr_perf_on;
static double hitch_ms = 25;      /* DKR_PERF_HITCH_MS: frames slower than this are reported one by one */
void dkr_perf_init(void) {
    dkr_perf_on = dkr_settings()->show_stats;
    const char *text = getenv("DKR_PERF_HITCH_MS");
    if (text && atof(text) > 0) hitch_ms = atof(text);
}
double dkr_perf_last_fps;
int64_t dkr_perf_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

/* One line for a slow frame: where its time went, from the counters added since the previous present. */
static void report_hitch(double frame_ms, double ms, const DkrPerf *now, const DkrPerf *before, int64_t frame) {
#define D(f) ((double)(now->f - before->f))
    fprintf(stderr, "perf: hitch frame %lld took %.1f ms | shader %.1f (%.0f compiled) pipeline %.1f (%.0f new) "
            "tex upload %.1f (%.0f) alloc %.1f (%.0f) gpu wait %.1f replay %.1f present %.1f pace %.1f\n",
            (long long)frame, frame_ms, D(shader) * ms, D(shader_compiles), D(pipeline) * ms, D(pipelines),
            D(upload) * ms, D(uploads), D(alloc) * ms, D(allocs), D(gpu_wait) * ms, D(replay) * ms,
            D(present) * ms, D(pace) * ms);
#undef D
}

void dkr_perf_frame(void) {
    static int64_t start, last, worst, frames, total_frames, hitches;
    static DkrPerf previous;
    if (!dkr_perf_on) return;
    int64_t now = dkr_perf_now();
    if (!start) { start = last = now; memset(&dkr_perf, 0, sizeof dkr_perf); previous = dkr_perf; return; }
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    double ms = 1000.0 / (double)f.QuadPart, frame_ms = (double)(now - last) * ms;
    total_frames++;
    if (frame_ms > hitch_ms) { hitches++; report_hitch(frame_ms, ms, &dkr_perf, &previous, total_frames); }
    previous = dkr_perf;
    if (now - last > worst) worst = now - last;
    last = now; frames++;
    if (now - start < f.QuadPart) return;
    double n = (double)frames, wall = (double)(now - start) * ms;
    DkrPerf p = dkr_perf;
    dkr_perf_last_fps = n * 1000.0 / wall;
    fprintf(stderr, "perf: %.1f fps, frame %.2f ms (worst %.2f, %lld hitches) | replay %.2f (gpu wait %.2f, shader key %.2f, "
            "shader %.2f, tex upload %.2f, pipeline %.2f, alloc %.2f) present %.2f pace %.2f | per frame: %.1f submits, "
            "%.0f draws, %.0f temp buffers, %.0f KiB, %.1f uploads | %lld new pipelines, %lld shaders compiled, %lld allocations\n",
            n * 1000.0 / wall, wall / n, (double)worst * ms, (long long)hitches, p.replay * ms / n, p.gpu_wait * ms / n,
            p.program * ms / n, p.shader * ms / n, p.upload * ms / n, p.pipeline * ms / n, p.alloc * ms / n,
            p.present * ms / n, p.pace * ms / n, p.submits / n, p.draws / n, p.temp_buffers / n, p.temp_bytes / 1024.0 / n,
            p.uploads / n, (long long)p.pipelines, (long long)p.shader_compiles, (long long)p.allocs);
    fflush(stderr);
    memset(&dkr_perf, 0, sizeof dkr_perf);
    previous = dkr_perf;
    start = now; worst = 0; frames = 0; hitches = 0;
}
