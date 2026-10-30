/* Differential test harness. Runs recompiled code on states written by tests/diff (Python) and writes the
 * resulting states back, so the Python side can compare them with Unicorn running the original A64 code.
 *
 * Input file (little-endian):
 *   "DKRT" u32 nregions u32 ncases
 *   nregions x { u64 addr, u64 size, u32 flags, u32 pad, size bytes }
 *     flags bit0: dump the region after each case; bit1: dump its FNV-1a 64 hash instead of its bytes
 *   ncases   x { u32 case_fn, u32 pad, u64 state[STATE_WORDS] }
 * Output file: ncases x { u64 state[STATE_WORDS], then each dumped region (bytes or 8-byte hash) }
 * State layout: x0..x30, sp, nzcv, tpidr, tpidrro, pc, then v0..v31 as two u64 each (low, high).
 * Every region is restored to its initial bytes before each case. */
#include "dkr/rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_WORDS 100

extern GuestFn dkr_case_table[];
extern const size_t dkr_case_count;

typedef struct Region {
    uint64_t addr, size;
    uint32_t flags;
    uint8_t *init;
} Region;

static void die(const char *m) {
    fprintf(stderr, "harness: %s\n", m);
    exit(2);
}

static void rd(void *p, size_t n, FILE *f) {
    if (fread(p, 1, n, f) != n) die("short read");
}

/* A deterministic SVC handler, mirrored by the Unicorn side (tools/dkrecomp/testing/difftest.py, on_intr), so
 * tests can check that recompiled code reaches the runtime at an SVC and resumes correctly after it. */
static void test_svc_hook(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)pc;
    c->x[0] = c->x[0] * 3 + imm;
    c->x[1] = 0xC0FFEE;
}

int main(int argc, char **argv) {
    if (argc != 3) die("usage: harness <in> <out>");
    dkr_svc_hook = test_svc_hook;
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out) die("cannot open files");
    char magic[4];
    uint32_t nreg, ncase;
    rd(magic, 4, in);
    if (memcmp(magic, "DKRT", 4)) die("bad magic");
    rd(&nreg, 4, in);
    rd(&ncase, 4, in);
    Region *regs = calloc(nreg, sizeof *regs);
    for (uint32_t i = 0; i < nreg; i++) {
        uint32_t pad;
        rd(&regs[i].addr, 8, in);
        rd(&regs[i].size, 8, in);
        rd(&regs[i].flags, 4, in);
        rd(&pad, 4, in);
        regs[i].init = malloc(regs[i].size);
        rd(regs[i].init, regs[i].size, in);
        if (rt_map_fixed(regs[i].addr, regs[i].size)) {
            fprintf(stderr, "harness: cannot map %#llx+%#llx (os error %lu)\n", (unsigned long long)regs[i].addr,
                    (unsigned long long)regs[i].size, rt_last_os_error());
            return 3;
        }
    }
    for (uint32_t k = 0; k < ncase; k++) {
        uint32_t fn, pad;
        uint64_t s[STATE_WORDS];
        rd(&fn, 4, in);
        rd(&pad, 4, in);
        rd(s, sizeof s, in);
        if (fn >= dkr_case_count) die("case function index out of range");
        if (getenv("DKR_TRACE")) { fprintf(stderr, "case %u fn %u\n", k, fn); fflush(stderr); }
        for (uint32_t i = 0; i < nreg; i++) memcpy((void *)(uintptr_t)regs[i].addr, regs[i].init, regs[i].size);
        Cpu c;
        memset(&c, 0, sizeof c);
        memcpy(c.x, s, 31 * 8);
        c.sp = s[31];
        c.nzcv = s[32];
        c.tpidr = s[33];
        c.tpidrro = s[34];
        c.pc = s[35];
        memcpy(c.v, s + 36, 32 * 16);
        dkr_case_table[fn](&c);
        memcpy(s, c.x, 31 * 8);
        s[31] = c.sp;
        s[32] = c.nzcv;
        s[33] = c.tpidr;
        s[34] = c.tpidrro;
        s[35] = c.pc;
        memcpy(s + 36, c.v, 32 * 16);
        fwrite(s, sizeof s, 1, out);
        for (uint32_t i = 0; i < nreg; i++) {
            if (!(regs[i].flags & 1)) continue;
            const uint8_t *p = (const uint8_t *)(uintptr_t)regs[i].addr;
            if (regs[i].flags & 2) { /* FNV-1a 64 of the region instead of its bytes */
                uint64_t h = 0xcbf29ce484222325ull;
                for (uint64_t j = 0; j < regs[i].size; j++) h = (h ^ p[j]) * 0x100000001b3ull;
                fwrite(&h, 8, 1, out);
            } else {
                fwrite(p, 1, regs[i].size, out);
            }
        }
    }
    fclose(out);
    return 0;
}
