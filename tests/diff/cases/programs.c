/* Test programs for the function-level differential test (tests/diff/func_diff.py).
 *
 * Compiled three ways: by Clang for AArch64 (the guest code DKRecomp recompiles), and natively for x86-64 (the
 * expected result). Everything is freestanding integer code; every test rebuilds its data from `seed`, so a
 * test's result does not depend on earlier calls. Entry point: test_entry(which, seed). */
#include <stddef.h>
#include <stdint.h>

#define NOINLINE __attribute__((noinline))

static uint64_t rng_state;
static NOINLINE uint64_t rng(void) { /* xorshift64* */
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1Dull;
}
static void seed_rng(uint64_t s) { rng_state = s ? s : 0x9E3779B97F4A7C15ull; }

static uint32_t arr32[257];
static uint8_t bytes[1024];
static int64_t arr64[64];

/* recursion and BL */
NOINLINE uint64_t fib(uint64_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

/* loops, loads, 32-bit arithmetic */
NOINLINE uint32_t sum_mix(const uint32_t *a, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s = (s ^ a[i]) * 16777619u + (a[i] >> 3);
    return s;
}

/* stores, nested loops, compares */
NOINLINE void insertion_sort(int64_t *a, size_t n) {
    for (size_t i = 1; i < n; i++) {
        int64_t v = a[i];
        size_t j = i;
        while (j > 0 && a[j - 1] > v) { a[j] = a[j - 1]; j--; }
        a[j] = v;
    }
}

/* byte loads/stores, sign extension */
NOINLINE int32_t byte_ops(uint8_t *b, size_t n) {
    int32_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        int8_t s = (int8_t)b[i];
        acc += s * (int32_t)(i & 7);
        b[i] = (uint8_t)(b[i] * 3 + 1);
    }
    return acc;
}

/* switch: Clang emits a jump table (ADR, LDRB/LDRH, ADD, BR) */
NOINLINE uint64_t dispatch_switch(uint64_t op, uint64_t a, uint64_t b) {
    switch (op & 15) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return a / (b | 1);
    case 4: return a % (b | 1);
    case 5: return a << (b & 63);
    case 6: return a >> (b & 63);
    case 7: return (uint64_t)((int64_t)a >> (b & 63));
    case 8: return a & b;
    case 9: return a | b;
    case 10: return a ^ b;
    case 11: return ~a;
    case 12: return (a > b) ? a : b;
    case 13: return ((int64_t)a < (int64_t)b) ? a : b;
    default: return a * 31 + b;
    }
}

/* 64x64->128 multiply (UMULH/SMULH), division, bit tricks */
NOINLINE uint64_t wide_math(uint64_t a, uint64_t b) {
    unsigned __int128 p = (unsigned __int128)a * b;
    __int128 q = (__int128)(int64_t)a * (int64_t)b;
    uint64_t r = (uint64_t)(p >> 64) ^ (uint64_t)p;
    r += (uint64_t)(q >> 64);
    r ^= (uint64_t)__builtin_clzll(a | 1) << 7;
    /* popcount by nibble table: Clang turns __builtin_popcountll and the usual loops into NEON CNT */
    static const uint8_t nib[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};
    for (int k = 0; k < 64; k += 4) r += nib[(b >> k) & 15];
    r ^= __builtin_bswap64(a);
    r += (int64_t)a / (int64_t)((b >> 1) | 1); /* divisor > 0: INT64_MIN / -1 would trap natively */
    r += (uint32_t)a / ((uint32_t)b | 1);
    return r;
}

/* function pointers: BLR and dispatch through the function table */
typedef uint64_t (*binop)(uint64_t, uint64_t);
static NOINLINE uint64_t op_add(uint64_t a, uint64_t b) { return a + b; }
static NOINLINE uint64_t op_mul(uint64_t a, uint64_t b) { return a * b + 1; }
static NOINLINE uint64_t op_rot(uint64_t a, uint64_t b) { return (a << (b & 63)) | (a >> ((64 - b) & 63)); }
static binop ops[3] = {op_add, op_mul, op_rot};
NOINLINE uint64_t fold_ptrs(uint64_t seed) {
    uint64_t acc = seed;
    for (int i = 0; i < 24; i++) acc = ops[(acc >> 7) % 3](acc, (uint64_t)i * 0x9E37);
    return acc;
}

/* struct copies: LDP/STP */
struct vec4 { int64_t x, y, z, w; };
NOINLINE struct vec4 vec_add(struct vec4 a, struct vec4 b) {
    struct vec4 r = {a.x + b.x, a.y - b.y, a.z * b.z, a.w ^ b.w};
    return r;
}

/* atomics without LSE: LDAXR/STLXR loops */
static volatile uint64_t counter64;
static volatile uint32_t counter32;
NOINLINE uint64_t atomics(uint64_t n) {
    counter64 = 0;
    counter32 = 0;
    for (uint64_t i = 0; i < n; i++) {
        __atomic_fetch_add(&counter64, i, __ATOMIC_SEQ_CST);
        __atomic_fetch_xor(&counter32, (uint32_t)(i * 7), __ATOMIC_ACQ_REL);
    }
    uint64_t expected = 5;
    __atomic_compare_exchange_n(&counter64, &expected, 9, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return counter64 * 1000003u + counter32;
}

/* conditional select / compare chains */
NOINLINE int64_t clamp_sum(const int64_t *a, size_t n, int64_t lo, int64_t hi) {
    int64_t s = 0;
    for (size_t i = 0; i < n; i++) {
        int64_t v = a[i];
        v = v < lo ? lo : v;
        v = v > hi ? hi : v;
        s += (v & 1) ? -v : v;
    }
    return s;
}

/* string-like scanning */
NOINLINE size_t count_runs(const uint8_t *b, size_t n) {
    size_t runs = 0;
    for (size_t i = 1; i < n; i++) runs += (b[i] & 3) != (b[i - 1] & 3);
    return runs;
}

/* SVC: the recompiled code must hand the call to the runtime's SVC hook and resume after it. The test handler
 * (tests/diff/harness.c test_svc_hook, mirrored for Unicorn) computes x0 = x0 * 3 + imm and clobbers x1. */
#ifdef __aarch64__
static NOINLINE uint64_t do_svc(uint64_t a) {
    register uint64_t x0 __asm__("x0") = a;
    __asm__ volatile("svc #0x26" : "+r"(x0) : : "x1", "memory");
    return x0;
}
#else
static NOINLINE uint64_t do_svc(uint64_t a) { return a * 3 + 0x26; } /* native model of the test handler */
#endif

/* Scalar floating point the way game code uses it: integration steps, clamps, conversions, square roots.
 * Built with -ffp-contract=off on both sides so the native reference does the same roundings; results stay
 * finite, because natively generated NaNs differ from A64's (that difference is covered by insn_diff). */
struct body { float x, y, vx, vy; };
static struct body bodies[16];
NOINLINE uint64_t fp_physics(uint64_t seed) {
    float g = -9.81f, dt = 1.0f / 60.0f;
    double energy = 0.0;
    for (int i = 0; i < 16; i++) {
        bodies[i].x = (float)(int32_t)(rng() & 0xFFFF) / 64.0f;
        bodies[i].y = (float)(rng() % 1000) * 0.25f;
        bodies[i].vx = (float)((int32_t)(rng() & 0xFF) - 128) * 0.1f;
        bodies[i].vy = 0.0f;
    }
    for (int step = 0; step < 120; step++) {
        for (int i = 0; i < 16; i++) {
            struct body *b = &bodies[i];
            b->vy += g * dt;
            b->x += b->vx * dt;
            b->y += b->vy * dt;
            if (b->y < 0.0f) { b->y = -b->y; b->vy = -b->vy * 0.8f; }
            float speed = __builtin_sqrtf(b->vx * b->vx + b->vy * b->vy);
            if (speed > 50.0f) { b->vx *= 50.0f / speed; b->vy *= 50.0f / speed; }
            energy += 0.5 * (double)speed * (double)speed + 9.81 * (double)b->y;
        }
    }
    uint64_t h = (uint64_t)(int64_t)(energy * 1000.0);
    for (int i = 0; i < 16; i++) {
        h = h * 31 + (uint64_t)(int64_t)bodies[i].x;
        h = h * 31 + (uint32_t)(int32_t)(bodies[i].y * 16.0f);
        h ^= (uint64_t)(bodies[i].vx > bodies[i].vy) << (i & 31);
        h += (uint64_t)__builtin_floorf(bodies[i].x / 3.0f) + (uint64_t)(seed & 7);
    }
    return h;
}

uint64_t test_entry(uint64_t which, uint64_t seed) {
    seed_rng(seed);
    for (int i = 0; i < 257; i++) arr32[i] = (uint32_t)rng();
    for (int i = 0; i < 1024; i++) bytes[i] = (uint8_t)rng();
    for (int i = 0; i < 64; i++) arr64[i] = (int64_t)rng() >> (rng() & 31);
    switch (which) {
    case 0: return fib(seed % 20);
    case 1: return sum_mix(arr32, 257);
    case 2: {
        insertion_sort(arr64, 64);
        uint64_t h = 0;
        for (int i = 0; i < 64; i++) h = h * 1099511628211ull ^ (uint64_t)arr64[i];
        return h;
    }
    case 3: {
        int32_t r = byte_ops(bytes, 1024);
        return (uint64_t)(int64_t)r ^ sum_mix((const uint32_t *)bytes, 256);
    }
    case 4: {
        uint64_t acc = 0;
        for (int i = 0; i < 64; i++) acc = acc * 7 + dispatch_switch(rng(), rng(), rng() & 0xFFFF);
        return acc;
    }
    case 5: return wide_math(rng(), rng()) ^ wide_math(seed, ~seed);
    case 6: return fold_ptrs(seed);
    case 7: {
        struct vec4 a = {(int64_t)rng(), (int64_t)rng(), (int64_t)rng(), (int64_t)rng()};
        struct vec4 b = {(int64_t)rng(), (int64_t)rng(), (int64_t)rng(), (int64_t)rng()};
        struct vec4 r = vec_add(a, b);
        return (uint64_t)(r.x ^ r.y ^ r.z ^ r.w);
    }
    case 8: return atomics(seed % 50 + 1);
    case 9: return (uint64_t)clamp_sum(arr64, 64, -(int64_t)(seed & 0xFFFFFF), (int64_t)(seed >> 40));
    case 10: return count_runs(bytes, 1024);
    case 11: return do_svc(seed) ^ do_svc(seed >> 3) * 5;
    case 12: return fp_physics(seed);
    default: return 0;
    }
}
#define TEST_COUNT 13
uint64_t test_count(void) { return TEST_COUNT; }

#ifdef __aarch64__
/* Entry for the linker; never called. */
void _start(void) {}
#endif
