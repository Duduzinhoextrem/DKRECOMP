/* DKRecomp guest CPU state and the helpers that recompiled code calls.
 *
 * Recompiled functions take a Cpu* and read/write guest registers through it. Guest memory addresses are host
 * addresses (the runtime maps guest memory at the same numeric address), so loads and stores are plain pointer
 * accesses. Everything here must compile with both MSVC (C11) and Clang. */
#ifndef DKR_CPU_H
#define DKR_CPU_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define DKR_MSVC 1
#define DKR_INLINE static __forceinline
#define DKR_NORETURN __declspec(noreturn)
#else
#define DKR_INLINE static inline __attribute__((always_inline))
#define DKR_NORETURN __attribute__((noreturn))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* One 128-bit SIMD&FP register. Lane i of each view is lane i of the A64 register (both little-endian). */
typedef union V128 {
    uint8_t b[16];
    uint16_t h[8];
    uint32_t s[4];
    uint64_t d[2];
} V128;

typedef struct Cpu {
    uint64_t x[32];     /* x0-x30; x[31] is a scratch slot that always reads back as 0 via the ZR macros */
    uint64_t sp;
    uint64_t pc;        /* target address for indirect dispatch */
    uint64_t nzcv;      /* flags in bits 31..28, the layout MRS NZCV returns */
    uint64_t tpidr;     /* TPIDR_EL0 */
    uint64_t tpidrro;   /* TPIDRRO_EL0 (thread local region pointer on Horizon) */
    uint64_t fpcr, fpsr;
    uint64_t excl_addr, excl_val; /* exclusive monitor (single address) */
    uint8_t excl_valid;
    uint8_t _pad[7];
    void *thread;       /* runtime thread object */
    V128 v[32];         /* V0-V31 */
} Cpu;

typedef void (*GuestFn)(Cpu *);

/* ---- runtime entry points (runtime/src) --------------------------------------------------------------- */
void rt_dispatch(Cpu *c);                                   /* call the guest function at c->pc */
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc);
DKR_NORETURN void rt_brk(Cpu *c, uint32_t imm, uint64_t pc);
DKR_NORETURN void rt_unsupported(Cpu *c, uint64_t pc, uint32_t word);
uint64_t rt_cntvct(void);
uint64_t rt_cntfrq(void);
void rt_fence(void);                                        /* full memory barrier (DMB/DSB, release stores) */

/* ---- memory: guest address == host address -------------------------------------------------------------- */
DKR_INLINE uint8_t ld8(uint64_t a) { return *(const uint8_t *)(uintptr_t)a; }
DKR_INLINE uint16_t ld16(uint64_t a) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)a, 2); return v; }
DKR_INLINE uint32_t ld32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
DKR_INLINE uint64_t ld64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
DKR_INLINE void st8(uint64_t a, uint64_t v) { *(uint8_t *)(uintptr_t)a = (uint8_t)v; }
DKR_INLINE void st16(uint64_t a, uint64_t v) { uint16_t t = (uint16_t)v; memcpy((void *)(uintptr_t)a, &t, 2); }
DKR_INLINE void st32(uint64_t a, uint64_t v) { uint32_t t = (uint32_t)v; memcpy((void *)(uintptr_t)a, &t, 4); }
DKR_INLINE void st64(uint64_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }

/* ---- flags ---------------------------------------------------------------------------------------------- */
#define DKR_N (1ull << 31)
#define DKR_Z (1ull << 30)
#define DKR_C (1ull << 29)
#define DKR_V (1ull << 28)

/* AddWithCarry() of the Arm ARM. Returns the result; stores NZCV when `setflags`. */
DKR_INLINE uint64_t awc64(Cpu *c, uint64_t a, uint64_t b, uint64_t cin, int setflags) {
    uint64_t r = a + b + cin;
    if (setflags) {
        uint64_t carry = (r < a) || (cin && r == a);
        uint64_t ovf = ((~(a ^ b)) & (a ^ r)) >> 63;
        c->nzcv = ((r >> 63) ? DKR_N : 0) | (r == 0 ? DKR_Z : 0) | (carry ? DKR_C : 0) | (ovf ? DKR_V : 0);
    }
    return r;
}
DKR_INLINE uint64_t awc32(Cpu *c, uint32_t a, uint32_t b, uint32_t cin, int setflags) {
    uint64_t wide = (uint64_t)a + b + cin;
    uint32_t r = (uint32_t)wide;
    if (setflags) {
        uint32_t ovf = ((~(a ^ b)) & (a ^ r)) >> 31;
        c->nzcv = (r & 0x80000000u ? DKR_N : 0) | (r == 0 ? DKR_Z : 0) | ((wide >> 32) ? DKR_C : 0) |
                  (ovf ? DKR_V : 0);
    }
    return r;
}
DKR_INLINE void flags_logic64(Cpu *c, uint64_t r) { c->nzcv = (r >> 63 ? DKR_N : 0) | (r == 0 ? DKR_Z : 0); }
DKR_INLINE void flags_logic32(Cpu *c, uint32_t r) { c->nzcv = (r >> 31 ? DKR_N : 0) | (r == 0 ? DKR_Z : 0); }

/* ConditionHolds() of the Arm ARM. */
DKR_INLINE int cond_holds(uint64_t f, int cond) {
    int n = (f & DKR_N) != 0, z = (f & DKR_Z) != 0, cy = (f & DKR_C) != 0, v = (f & DKR_V) != 0, r;
    switch (cond >> 1) {
    case 0: r = z; break;
    case 1: r = cy; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = cy && !z; break;
    case 5: r = n == v; break;
    case 6: r = (n == v) && !z; break;
    default: r = 1; break;
    }
    if ((cond & 1) && cond != 15) r = !r;
    return r;
}

/* ---- arithmetic that differs between C and A64 --------------------------------------------------------- */
DKR_INLINE uint64_t ror64(uint64_t v, unsigned r) { r &= 63; return r ? (v >> r) | (v << (64 - r)) : v; }
DKR_INLINE uint32_t ror32(uint32_t v, unsigned r) { r &= 31; return r ? (v >> r) | (v << (32 - r)) : v; }
DKR_INLINE uint64_t asr64(uint64_t v, unsigned s) { s &= 63; return (uint64_t)((int64_t)v >> s); }
DKR_INLINE uint32_t asr32(uint32_t v, unsigned s) { s &= 31; return (uint32_t)((int32_t)v >> s); }

/* A64 division never traps: x/0 == 0 and INT_MIN/-1 == INT_MIN. x86 IDIV would raise #DE on both. */
DKR_INLINE uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
DKR_INLINE uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
DKR_INLINE uint64_t sdiv64(uint64_t a, uint64_t b) {
    if (!b) return 0;
    if (a == 0x8000000000000000ull && b == ~0ull) return a;
    return (uint64_t)((int64_t)a / (int64_t)b);
}
DKR_INLINE uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (!b) return 0;
    if (a == 0x80000000u && b == ~0u) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}

DKR_INLINE uint64_t umulh64(uint64_t a, uint64_t b) {
#ifdef DKR_MSVC
    return __umulh(a, b);
#else
    return (uint64_t)(((unsigned __int128)a * b) >> 64);
#endif
}
DKR_INLINE uint64_t smulh64(uint64_t a, uint64_t b) {
#ifdef DKR_MSVC
    return (uint64_t)__mulh((int64_t)a, (int64_t)b);
#else
    return (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64);
#endif
}

DKR_INLINE uint64_t clz64(uint64_t v) {
    uint64_t n = 0;
    if (!v) return 64;
    while (!(v & 0x8000000000000000ull)) { v <<= 1; n++; }
    return n;
}
DKR_INLINE uint64_t clz32(uint32_t v) { return v ? clz64((uint64_t)v << 32) : 32; }
/* CountLeadingSignBits(x) = CLZ of the (N-1)-bit value x<N-1:1> EOR x<N-2:0>. */
DKR_INLINE uint64_t cls64(uint64_t v) { return clz64((v ^ (v << 1)) | 1); }
DKR_INLINE uint64_t cls32(uint32_t v) { return clz32((uint32_t)(v ^ (v << 1)) | 1u); }
DKR_INLINE uint64_t rbit64(uint64_t v) {
    uint64_t r = 0;
    for (int i = 0; i < 64; i++) { r = (r << 1) | (v & 1); v >>= 1; }
    return r;
}
DKR_INLINE uint32_t rbit32(uint32_t v) { return (uint32_t)(rbit64(v) >> 32); }
DKR_INLINE uint64_t bswap64(uint64_t v) {
    v = ((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull);
    v = ((v & 0x0000FFFF0000FFFFull) << 16) | ((v >> 16) & 0x0000FFFF0000FFFFull);
    return (v << 32) | (v >> 32);
}
/* Reverse the bytes inside each `container`-bit lane of a 64-bit value. */
DKR_INLINE uint64_t rev_lanes(uint64_t v, int container) {
    if (container == 64) return bswap64(v);
    if (container == 32) {
        uint64_t s = bswap64(v);
        return (s << 32) | (s >> 32);
    }
    return ((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull);
}

/* ---- SIMD&FP register moves ----------------------------------------------------------------------------- */
/* A scalar load of n < 16 bytes writes the low bytes and clears the rest of the register. */
DKR_INLINE void vload(Cpu *c, int t, uint64_t a, int n) {
    V128 r;
    memset(&r, 0, sizeof r);
    memcpy(r.b, (const void *)(uintptr_t)a, (size_t)n);
    c->v[t] = r;
}
DKR_INLINE void vstore(Cpu *c, int t, uint64_t a, int n) { memcpy((void *)(uintptr_t)a, c->v[t].b, (size_t)n); }
DKR_INLINE uint64_t velem(const Cpu *c, int r, int esize, int i) {
    switch (esize) {
    case 1: return c->v[r].b[i];
    case 2: return c->v[r].h[i];
    case 4: return c->v[r].s[i];
    default: return c->v[r].d[i];
    }
}
DKR_INLINE void vset_elem(Cpu *c, int r, int esize, int i, uint64_t x) {
    switch (esize) {
    case 1: c->v[r].b[i] = (uint8_t)x; break;
    case 2: c->v[r].h[i] = (uint16_t)x; break;
    case 4: c->v[r].s[i] = (uint32_t)x; break;
    default: c->v[r].d[i] = x; break;
    }
}
/* Replicate the low `esize` bytes of x into a 64-bit lane pattern. */
DKR_INLINE uint64_t vrep(uint64_t x, int esize) {
    switch (esize) {
    case 1: return (x & 0xFF) * 0x0101010101010101ull;
    case 2: return (x & 0xFFFF) * 0x0001000100010001ull;
    case 4: return (x & 0xFFFFFFFFull) * 0x0000000100000001ull;
    default: return x;
    }
}

/* ---- exclusive monitor (one address per Cpu, compare-and-swap at the store) ---------------------------- */
DKR_INLINE uint64_t ldx(Cpu *c, uint64_t a, int n) {
    uint64_t v = n == 1 ? ld8(a) : n == 2 ? ld16(a) : n == 4 ? ld32(a) : ld64(a);
    c->excl_addr = a;
    c->excl_val = v;
    c->excl_valid = 1;
    return v;
}
int rt_cas(uint64_t addr, uint64_t expect, uint64_t value, int nbytes); /* runtime/src/atomics.c */
/* Returns the STXR status: 0 stored, 1 failed. */
DKR_INLINE uint64_t stx(Cpu *c, uint64_t a, uint64_t v, int n) {
    int ok = c->excl_valid && c->excl_addr == a && rt_cas(a, c->excl_val, v, n);
    c->excl_valid = 0;
    return ok ? 0 : 1;
}

#include "dkr/fp.h"
#include "dkr/simd.h"

#ifdef __cplusplus
}
#endif
#endif
