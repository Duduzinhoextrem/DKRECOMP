/* DKRecomp scalar floating-point helpers with A64 semantics (included by cpu.h).
 *
 * Values travel as raw bits (uint32_t single, uint64_t double). The host computes with SSE (IEEE 754, round to
 * nearest), which matches A64 for ordinary results; these helpers handle where A64 and x86 differ:
 *  - NaN operands: A64 returns the first signalling NaN (quietened), else the first quiet NaN, in operand order
 *    (FPProcessNaNs). x86 picks by position only, and compilers may swap commutative operands.
 *  - NaNs created from non-NaN operands (inf-inf, 0*inf, 0/0, sqrt(-x)): A64 gives the default NaN 0x7FC00000;
 *    x86 gives the negative "indefinite" NaN 0xFFC00000.
 *  - float -> integer: A64 rounds as the instruction says and saturates (NaN -> 0); C casts are undefined there.
 *  - FMA is fused (single rounding), so it needs a real fused multiply-add.
 * Only FPCR = 0 is modelled (round to nearest, no flush-to-zero, no default-NaN mode); FPSR flags are not kept.
 * Arm ARM references: FPProcessNaNs, FPMulAdd, FPMax/FPMin/FPMaxNum/FPMinNum, FPCompare, FPToFixed, FPConvertNaN. */
#ifndef DKR_FP_H
#define DKR_FP_H

#include <math.h>

#if defined(_MSC_VER) && !defined(__clang__)
#pragma fp_contract(off)
#else
#pragma STDC FP_CONTRACT OFF
#endif

/* ---- bit casts and classification ---------------------------------------------------------------------- */
DKR_INLINE float f32_of(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
DKR_INLINE uint32_t bits32(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
DKR_INLINE double f64_of(uint64_t b) { double f; memcpy(&f, &b, 8); return f; }
DKR_INLINE uint64_t bits64(double f) { uint64_t b; memcpy(&b, &f, 8); return b; }

#define F32_SIGN 0x80000000u
#define F64_SIGN 0x8000000000000000ull
#define F32_DNAN 0x7FC00000u
#define F64_DNAN 0x7FF8000000000000ull
#define F32_QBIT 0x00400000u
#define F64_QBIT 0x0008000000000000ull

DKR_INLINE int nan32(uint32_t b) { return (b & 0x7FFFFFFFu) > 0x7F800000u; }
DKR_INLINE int nan64(uint64_t b) { return (b & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
DKR_INLINE int snan32(uint32_t b) { return nan32(b) && !(b & F32_QBIT); }
DKR_INLINE int snan64(uint64_t b) { return nan64(b) && !(b & F64_QBIT); }
DKR_INLINE int qnan32(uint32_t b) { return nan32(b) && (b & F32_QBIT); }
DKR_INLINE int qnan64(uint64_t b) { return nan64(b) && (b & F64_QBIT); }
DKR_INLINE int inf32(uint32_t b) { return (b & 0x7FFFFFFFu) == 0x7F800000u; }
DKR_INLINE int inf64(uint64_t b) { return (b & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull; }
DKR_INLINE int zero32(uint32_t b) { return (b & 0x7FFFFFFFu) == 0; }
DKR_INLINE int zero64(uint64_t b) { return (b & 0x7FFFFFFFFFFFFFFFull) == 0; }

/* FPProcessNaNs / FPProcessNaNs3: 1 and *r set when a NaN operand decides the result. */
DKR_INLINE int nans2_32(uint32_t a, uint32_t b, uint32_t *r) {
    if (snan32(a)) { *r = a | F32_QBIT; return 1; }
    if (snan32(b)) { *r = b | F32_QBIT; return 1; }
    if (nan32(a)) { *r = a; return 1; }
    if (nan32(b)) { *r = b; return 1; }
    return 0;
}
DKR_INLINE int nans2_64(uint64_t a, uint64_t b, uint64_t *r) {
    if (snan64(a)) { *r = a | F64_QBIT; return 1; }
    if (snan64(b)) { *r = b | F64_QBIT; return 1; }
    if (nan64(a)) { *r = a; return 1; }
    if (nan64(b)) { *r = b; return 1; }
    return 0;
}
DKR_INLINE int nans3_32(uint32_t a, uint32_t b, uint32_t c, uint32_t *r) {
    if (snan32(a)) { *r = a | F32_QBIT; return 1; }
    if (snan32(b)) { *r = b | F32_QBIT; return 1; }
    if (snan32(c)) { *r = c | F32_QBIT; return 1; }
    if (nan32(a)) { *r = a; return 1; }
    if (nan32(b)) { *r = b; return 1; }
    if (nan32(c)) { *r = c; return 1; }
    return 0;
}
DKR_INLINE int nans3_64(uint64_t a, uint64_t b, uint64_t c, uint64_t *r) {
    if (snan64(a)) { *r = a | F64_QBIT; return 1; }
    if (snan64(b)) { *r = b | F64_QBIT; return 1; }
    if (snan64(c)) { *r = c | F64_QBIT; return 1; }
    if (nan64(a)) { *r = a; return 1; }
    if (nan64(b)) { *r = b; return 1; }
    if (nan64(c)) { *r = c; return 1; }
    return 0;
}
/* A result NaN that did not come from a NaN operand is the A64 default NaN. */
DKR_INLINE uint32_t gen32(float f) { uint32_t b = bits32(f); return nan32(b) ? F32_DNAN : b; }
DKR_INLINE uint64_t gen64(double f) { uint64_t b = bits64(f); return nan64(b) ? F64_DNAN : b; }

/* ---- arithmetic ----------------------------------------------------------------------------------------- */
#define DKR_FP_BIN(name, OP)                                                                             \
    DKR_INLINE uint32_t name##32(uint32_t a, uint32_t b) {                                               \
        uint32_t r;                                                                                      \
        if (nans2_32(a, b, &r)) return r;                                                                \
        return gen32(f32_of(a) OP f32_of(b));                                                            \
    }                                                                                                    \
    DKR_INLINE uint64_t name##64(uint64_t a, uint64_t b) {                                               \
        uint64_t r;                                                                                      \
        if (nans2_64(a, b, &r)) return r;                                                                \
        return gen64(f64_of(a) OP f64_of(b));                                                            \
    }
DKR_FP_BIN(fadd, +)
DKR_FP_BIN(fsub, -)
DKR_FP_BIN(fmul, *)
DKR_FP_BIN(fdiv, /)
#undef DKR_FP_BIN

/* FPMax/FPMin: NaNs propagate; max(+0,-0) = +0 and min(+0,-0) = -0. */
DKR_INLINE uint32_t fmax32(uint32_t a, uint32_t b) {
    uint32_t r;
    if (nans2_32(a, b, &r)) return r;
    if (f32_of(a) > f32_of(b)) return a;
    if (f32_of(b) > f32_of(a)) return b;
    return a & b;
}
DKR_INLINE uint32_t fmin32(uint32_t a, uint32_t b) {
    uint32_t r;
    if (nans2_32(a, b, &r)) return r;
    if (f32_of(a) < f32_of(b)) return a;
    if (f32_of(b) < f32_of(a)) return b;
    return a | b;
}
DKR_INLINE uint64_t fmax64(uint64_t a, uint64_t b) {
    uint64_t r;
    if (nans2_64(a, b, &r)) return r;
    if (f64_of(a) > f64_of(b)) return a;
    if (f64_of(b) > f64_of(a)) return b;
    return a & b;
}
DKR_INLINE uint64_t fmin64(uint64_t a, uint64_t b) {
    uint64_t r;
    if (nans2_64(a, b, &r)) return r;
    if (f64_of(a) < f64_of(b)) return a;
    if (f64_of(b) < f64_of(a)) return b;
    return a | b;
}
/* FPMaxNum/FPMinNum: a quiet NaN loses against anything that is not a quiet NaN. */
DKR_INLINE uint32_t fmaxnm32(uint32_t a, uint32_t b) {
    if (qnan32(a) && !qnan32(b)) a = 0xFF800000u;
    else if (!qnan32(a) && qnan32(b)) b = 0xFF800000u;
    return fmax32(a, b);
}
DKR_INLINE uint32_t fminnm32(uint32_t a, uint32_t b) {
    if (qnan32(a) && !qnan32(b)) a = 0x7F800000u;
    else if (!qnan32(a) && qnan32(b)) b = 0x7F800000u;
    return fmin32(a, b);
}
DKR_INLINE uint64_t fmaxnm64(uint64_t a, uint64_t b) {
    if (qnan64(a) && !qnan64(b)) a = 0xFFF0000000000000ull;
    else if (!qnan64(a) && qnan64(b)) b = 0xFFF0000000000000ull;
    return fmax64(a, b);
}
DKR_INLINE uint64_t fminnm64(uint64_t a, uint64_t b) {
    if (qnan64(a) && !qnan64(b)) a = 0x7FF0000000000000ull;
    else if (!qnan64(a) && qnan64(b)) b = 0x7FF0000000000000ull;
    return fmin64(a, b);
}

/* FPMulAdd(addend, op1, op2) = addend + op1 * op2 with one rounding. */
DKR_INLINE uint32_t fmadd32(uint32_t addend, uint32_t n, uint32_t m) {
    uint32_t r;
    if (qnan32(addend) && ((inf32(n) && zero32(m)) || (zero32(n) && inf32(m)))) return F32_DNAN;
    if (nans3_32(addend, n, m, &r)) return r;
#if defined(__clang__) || defined(__GNUC__)
    return gen32(__builtin_fmaf(f32_of(n), f32_of(m), f32_of(addend)));
#else
    return gen32(fmaf(f32_of(n), f32_of(m), f32_of(addend)));
#endif
}
DKR_INLINE uint64_t fmadd64(uint64_t addend, uint64_t n, uint64_t m) {
    uint64_t r;
    if (qnan64(addend) && ((inf64(n) && zero64(m)) || (zero64(n) && inf64(m)))) return F64_DNAN;
    if (nans3_64(addend, n, m, &r)) return r;
#if defined(__clang__) || defined(__GNUC__)
    return gen64(__builtin_fma(f64_of(n), f64_of(m), f64_of(addend)));
#else
    return gen64(fma(f64_of(n), f64_of(m), f64_of(addend)));
#endif
}

DKR_INLINE uint32_t fsqrt32(uint32_t a) {
    if (nan32(a)) return a | F32_QBIT;
    if ((a & F32_SIGN) && !zero32(a)) return F32_DNAN;
    return bits32(sqrtf(f32_of(a)));
}
DKR_INLINE uint64_t fsqrt64(uint64_t a) {
    if (nan64(a)) return a | F64_QBIT;
    if ((a & F64_SIGN) && !zero64(a)) return F64_DNAN;
    return bits64(sqrt(f64_of(a)));
}

/* ---- comparison: NZCV as FPCompare sets it ------------------------------------------------------------- */
DKR_INLINE uint64_t fcmp32(uint32_t a, uint32_t b) {
    if (nan32(a) || nan32(b)) return 0x30000000ull; /* unordered: C V */
    if (f32_of(a) == f32_of(b)) return 0x60000000ull; /* Z C */
    if (f32_of(a) < f32_of(b)) return 0x80000000ull;  /* N */
    return 0x20000000ull;                            /* C */
}
DKR_INLINE uint64_t fcmp64(uint64_t a, uint64_t b) {
    if (nan64(a) || nan64(b)) return 0x30000000ull;
    if (f64_of(a) == f64_of(b)) return 0x60000000ull;
    if (f64_of(a) < f64_of(b)) return 0x80000000ull;
    return 0x20000000ull;
}

/* ---- rounding to integral (FRINT*, and the rounding step of FCVT*) ----------------------------------- */
enum { DKR_RN = 0, DKR_RP = 1, DKR_RM = 2, DKR_RZ = 3, DKR_RA = 4 };
DKR_INLINE double fround(double x, int mode) {
    double f, d, r;
    switch (mode) {
    case DKR_RP: return ceil(x);
    case DKR_RM: return floor(x);
    case DKR_RZ: return trunc(x);
    case DKR_RA: return round(x);
    default: /* ties to even; |x| >= 2^52 is already integral, so x - floor(x) is exact */
        f = floor(x);
        d = x - f;
        r = (d > 0.5 || (d == 0.5 && fmod(f, 2.0) != 0.0)) ? f + 1.0 : f;
        return copysign(r, x);
    }
}
DKR_INLINE uint32_t frint32(uint32_t a, int mode) {
    if (nan32(a)) return a | F32_QBIT;
    return bits32((float)fround((double)f32_of(a), mode));
}
DKR_INLINE uint64_t frint64(uint64_t a, int mode) {
    if (nan64(a)) return a | F64_QBIT;
    return bits64(fround(f64_of(a), mode));
}

/* ---- conversions ---------------------------------------------------------------------------------------- */
/* FPConvertNaN keeps the sign and the top fraction bits and sets the quiet bit. */
DKR_INLINE uint64_t fcvt_s2d(uint32_t a) {
    if (nan32(a)) return ((uint64_t)(a >> 31) << 63) | F64_DNAN | ((uint64_t)(a & 0x003FFFFFu) << 29);
    return bits64((double)f32_of(a));
}
DKR_INLINE uint32_t fcvt_d2s(uint64_t a) {
    if (nan64(a)) return ((uint32_t)(a >> 63) << 31) | F32_DNAN | (uint32_t)((a >> 29) & 0x003FFFFFu);
    return bits32((float)f64_of(a));
}
/* FPToFixed with no fraction bits: round with `mode`, then saturate to the destination range. */
DKR_INLINE uint64_t fcvt_to_int(double x, int mode, int is_unsigned, int nbits) {
    double r;
    if (x != x) return 0;
    r = fround(x, mode);
    if (is_unsigned) {
        double lim = nbits == 64 ? 18446744073709551616.0 : 4294967296.0;
        if (r <= 0.0) return 0;
        if (r >= lim) return nbits == 64 ? ~0ull : 0xFFFFFFFFull;
        return (uint64_t)r;
    } else {
        double lim = nbits == 64 ? 9223372036854775808.0 : 2147483648.0;
        if (r >= lim) return nbits == 64 ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFull;
        if (r < -lim) return nbits == 64 ? 0x8000000000000000ull : 0xFFFFFFFF80000000ull;
        return (uint64_t)(int64_t)r;
    }
}
DKR_INLINE uint32_t scvtf32(int64_t v) { return bits32((float)v); }
DKR_INLINE uint64_t scvtf64(int64_t v) { return bits64((double)v); }
DKR_INLINE uint32_t ucvtf32(uint64_t v) { return bits32((float)v); }
DKR_INLINE uint64_t ucvtf64(uint64_t v) { return bits64((double)v); }
/* Fixed point: value / 2^fbits. The integer is rounded once when converted; dividing by a power of two is then
 * exact, because |value| >= 2^-64 stays a normal number in both formats. */
DKR_INLINE uint32_t scvtfx32(int64_t v, int fbits) { return bits32(ldexpf((float)v, -fbits)); }
DKR_INLINE uint64_t scvtfx64(int64_t v, int fbits) { return bits64(ldexp((double)v, -fbits)); }
DKR_INLINE uint32_t ucvtfx32(uint64_t v, int fbits) { return bits32(ldexpf((float)v, -fbits)); }
DKR_INLINE uint64_t ucvtfx64(uint64_t v, int fbits) { return bits64(ldexp((double)v, -fbits)); }

/* ---- scalar writes clear the rest of the vector register ---------------------------------------------- */
DKR_INLINE void vset_s(Cpu *c, int d, uint32_t v) { c->v[d].d[0] = v; c->v[d].d[1] = 0; }
DKR_INLINE void vset_d(Cpu *c, int d, uint64_t v) { c->v[d].d[0] = v; c->v[d].d[1] = 0; }

#endif
