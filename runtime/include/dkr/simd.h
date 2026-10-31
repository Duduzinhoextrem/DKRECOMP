/* DKRecomp Advanced SIMD helpers (included by cpu.h after fp.h).
 *
 * Every helper takes whole 128-bit values and returns the result register, so register aliasing (Vd == Vn) is
 * harmless. `lanes` is the number of active elements (Q=0 uses the low 64 bits and the result's high 64 bits are
 * zero). Element sizes are in bytes. These loops favour clarity over speed; the generated code can specialise
 * them later without changing the semantics checked by the differential tests. */
#ifndef DKR_SIMD_H
#define DKR_SIMD_H

DKR_INLINE uint64_t vget(const V128 *v, int esize, int i) {
    switch (esize) {
    case 1: return v->b[i];
    case 2: return v->h[i];
    case 4: return v->s[i];
    default: return v->d[i];
    }
}
DKR_INLINE void vput(V128 *v, int esize, int i, uint64_t x) {
    switch (esize) {
    case 1: v->b[i] = (uint8_t)x; break;
    case 2: v->h[i] = (uint16_t)x; break;
    case 4: v->s[i] = (uint32_t)x; break;
    default: v->d[i] = x; break;
    }
}
DKR_INLINE int64_t sext_e(uint64_t x, int esize) {
    int sh = 64 - 8 * esize;
    return sh ? (int64_t)(x << sh) >> sh : (int64_t)x;
}
DKR_INLINE uint64_t emask(int esize) { return esize == 8 ? ~0ull : ((1ull << (8 * esize)) - 1); }
DKR_INLINE V128 vzero(void) { V128 r; memset(&r, 0, sizeof r); return r; }

/* ---- reciprocal estimates (Arm ARM RecipEstimate / RecipSqrtEstimate, FPRecipEstimate / FPRSqrtEstimate) -- */
DKR_INLINE int recip_estimate(int a) { /* 256 <= a < 512 */
    a = a * 2 + 1;
    int b = (1 << 19) / a;
    return (b + 1) / 2;
}
DKR_INLINE int rsqrt_estimate(int a) { /* 128 <= a < 512 */
    if (a < 256) a = a * 2 + 1;
    else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    int b = 512;
    while ((int64_t)a * (b + 1) * (b + 1) < (1ll << 28)) b++;
    return (b + 1) / 2;
}
/* `frac52` is the significand aligned to 52 bits, `exp` the biased exponent, `n` 32 or 64. */
DKR_INLINE uint64_t frecpe_core(uint64_t sign, int exp, uint64_t frac52, int n) {
    if (exp == 0) {
        if (!(frac52 >> 51 & 1)) { exp = -1; frac52 = (frac52 << 2) & 0xFFFFFFFFFFFFFull; }
        else frac52 = (frac52 << 1) & 0xFFFFFFFFFFFFFull;
    }
    int scaled = (int)(0x100 | (frac52 >> 44));
    int rexp = (n == 32 ? 253 : 2045) - exp;
    uint64_t est = (uint64_t)recip_estimate(scaled);
    uint64_t f = (est & 0xFF) << 44;
    if (rexp == 0) f = (1ull << 51) | (f >> 1);
    else if (rexp == -1) { f = (1ull << 50) | (f >> 2); rexp = 0; }
    if (n == 32) return (sign << 31) | ((uint64_t)(rexp & 0xFF) << 23) | (f >> 29);
    return (sign << 63) | ((uint64_t)(rexp & 0x7FF) << 52) | f;
}
DKR_INLINE uint32_t frecpe32(uint32_t a) {
    uint32_t sign = a >> 31;
    if (nan32(a)) return a | F32_QBIT;
    if (inf32(a)) return sign << 31;
    if (zero32(a)) return (sign << 31) | 0x7F800000u;
    if ((a & 0x7FFFFFFFu) < 0x00200000u) return (sign << 31) | 0x7F800000u; /* |x| < 2^-128: overflow */
    return (uint32_t)frecpe_core(sign, (a >> 23) & 0xFF, (uint64_t)(a & 0x7FFFFF) << 29, 32);
}
DKR_INLINE uint64_t frecpe64(uint64_t a) {
    uint64_t sign = a >> 63;
    if (nan64(a)) return a | F64_QBIT;
    if (inf64(a)) return sign << 63;
    if (zero64(a)) return (sign << 63) | 0x7FF0000000000000ull;
    if ((a & 0x7FFFFFFFFFFFFFFFull) < 0x0004000000000000ull) return (sign << 63) | 0x7FF0000000000000ull;
    return frecpe_core(sign, (int)((a >> 52) & 0x7FF), a & 0xFFFFFFFFFFFFFull, 64);
}
DKR_INLINE uint64_t frsqrte_core(int exp, uint64_t frac52, int n) {
    if (exp == 0) {
        while (!(frac52 >> 51 & 1)) { frac52 = (frac52 << 1) & 0xFFFFFFFFFFFFFull; exp--; }
        frac52 = (frac52 << 1) & 0xFFFFFFFFFFFFFull;
    }
    int scaled = (exp & 1) == 0 ? (int)(0x100 | (frac52 >> 44)) : (int)(0x80 | (frac52 >> 45));
    int rexp = ((n == 32 ? 380 : 3068) - exp) / 2;
    uint64_t est = (uint64_t)rsqrt_estimate(scaled);
    if (n == 32) return ((uint64_t)(rexp & 0xFF) << 23) | ((est & 0xFF) << 15);
    return ((uint64_t)(rexp & 0x7FF) << 52) | ((est & 0xFF) << 44);
}
DKR_INLINE uint32_t frsqrte32(uint32_t a) {
    if (nan32(a)) return a | F32_QBIT;
    if (zero32(a)) return (a & F32_SIGN) | 0x7F800000u;
    if (a & F32_SIGN) return F32_DNAN;
    if (inf32(a)) return 0;
    return (uint32_t)frsqrte_core((a >> 23) & 0xFF, (uint64_t)(a & 0x7FFFFF) << 29, 32);
}
DKR_INLINE uint64_t frsqrte64(uint64_t a) {
    if (nan64(a)) return a | F64_QBIT;
    if (zero64(a)) return (a & F64_SIGN) | 0x7FF0000000000000ull;
    if (a & F64_SIGN) return F64_DNAN;
    if (inf64(a)) return 0;
    return frsqrte_core((int)((a >> 52) & 0x7FF), a & 0xFFFFFFFFFFFFFull, 64);
}

/* FPRecipStepFused: 2 - a*b; FPRSqrtStepFused: (3 - a*b) / 2. Both negate a before NaN processing. */
DKR_INLINE uint32_t frecps32(uint32_t a, uint32_t b) {
    uint32_t r;
    a ^= F32_SIGN;
    if (nans2_32(a, b, &r)) return r;
    if ((inf32(a) && zero32(b)) || (zero32(a) && inf32(b))) return 0x40000000u; /* 2.0 */
    if (inf32(a) || inf32(b)) return ((a ^ b) & F32_SIGN) | 0x7F800000u;
    return gen32(DKR_FMAF(f32_of(a), f32_of(b), 2.0f));
}
DKR_INLINE uint64_t frecps64(uint64_t a, uint64_t b) {
    uint64_t r;
    a ^= F64_SIGN;
    if (nans2_64(a, b, &r)) return r;
    if ((inf64(a) && zero64(b)) || (zero64(a) && inf64(b))) return 0x4000000000000000ull;
    if (inf64(a) || inf64(b)) return ((a ^ b) & F64_SIGN) | 0x7FF0000000000000ull;
    return gen64(DKR_FMA(f64_of(a), f64_of(b), 2.0));
}
DKR_INLINE uint32_t frsqrts32(uint32_t a, uint32_t b) {
    uint32_t r;
    a ^= F32_SIGN;
    if (nans2_32(a, b, &r)) return r;
    if ((inf32(a) && zero32(b)) || (zero32(a) && inf32(b))) return 0x3FC00000u; /* 1.5 */
    if (inf32(a) || inf32(b)) return ((a ^ b) & F32_SIGN) | 0x7F800000u;
    /* (3 - a*b)/2 with one rounding: halve a normal operand first (exact), so 3 - a*b cannot overflow before
     * the division. With both operands tiny the product is tiny and halving the sum is exact. */
    if (((b >> 23) & 0xFF) > 1) return gen32(DKR_FMAF(f32_of(a), f32_of(b) * 0.5f, 1.5f));
    if (((a >> 23) & 0xFF) > 1) return gen32(DKR_FMAF(f32_of(a) * 0.5f, f32_of(b), 1.5f));
    return gen32(DKR_FMAF(f32_of(a), f32_of(b), 3.0f) * 0.5f);
}
DKR_INLINE uint64_t frsqrts64(uint64_t a, uint64_t b) {
    uint64_t r;
    a ^= F64_SIGN;
    if (nans2_64(a, b, &r)) return r;
    if ((inf64(a) && zero64(b)) || (zero64(a) && inf64(b))) return 0x3FF8000000000000ull;
    if (inf64(a) || inf64(b)) return ((a ^ b) & F64_SIGN) | 0x7FF0000000000000ull;
    if (((b >> 52) & 0x7FF) > 1) return gen64(DKR_FMA(f64_of(a), f64_of(b) * 0.5, 1.5));
    if (((a >> 52) & 0x7FF) > 1) return gen64(DKR_FMA(f64_of(a) * 0.5, f64_of(b), 1.5));
    return gen64(DKR_FMA(f64_of(a), f64_of(b), 3.0) * 0.5);
}
/* FMULX: like FMUL, but inf * 0 = 2.0 with the XOR of the signs. */
DKR_INLINE uint32_t fmulx32(uint32_t a, uint32_t b) {
    uint32_t r;
    if (nans2_32(a, b, &r)) return r;
    if ((inf32(a) && zero32(b)) || (zero32(a) && inf32(b))) return ((a ^ b) & F32_SIGN) | 0x40000000u;
    return fmul32(a, b);
}
DKR_INLINE uint64_t fmulx64(uint64_t a, uint64_t b) {
    uint64_t r;
    if (nans2_64(a, b, &r)) return r;
    if ((inf64(a) && zero64(b)) || (zero64(a) && inf64(b))) return ((a ^ b) & F64_SIGN) | 0x4000000000000000ull;
    return fmul64(a, b);
}
/* Vector FP compares: all ones when true; any NaN makes the comparison false. */
DKR_INLINE uint64_t fcm32(uint32_t a, uint32_t b, int kind) {
    if (nan32(a) || nan32(b)) return 0;
    float x = f32_of(a), y = f32_of(b);
    int t = kind == 0 ? x == y : kind == 1 ? x >= y : kind == 2 ? x > y : kind == 3 ? x <= y : x < y;
    return t ? 0xFFFFFFFFull : 0;
}
DKR_INLINE uint64_t fcm64(uint64_t a, uint64_t b, int kind) {
    if (nan64(a) || nan64(b)) return 0;
    double x = f64_of(a), y = f64_of(b);
    int t = kind == 0 ? x == y : kind == 1 ? x >= y : kind == 2 ? x > y : kind == 3 ? x <= y : x < y;
    return t ? ~0ull : 0;
}

/* ---- FP vector operations: kind numbers are the lifter's (recomp/lifter.py VFP_KINDS) ------------------- */
enum { VF_ADD, VF_SUB, VF_MUL, VF_DIV, VF_MAX, VF_MIN, VF_MAXNM, VF_MINNM, VF_MULX, VF_RECPS, VF_RSQRTS,
       VF_CMEQ, VF_CMGE, VF_CMGT, VF_CMLE, VF_CMLT, VF_ABD };
DKR_INLINE uint64_t vf_op(int kind, uint64_t a, uint64_t b, int dbl) {
    if (!dbl) {
        uint32_t x = (uint32_t)a, y = (uint32_t)b;
        switch (kind) {
        case VF_ADD: return fadd32(x, y);
        case VF_SUB: return fsub32(x, y);
        case VF_MUL: return fmul32(x, y);
        case VF_DIV: return fdiv32(x, y);
        case VF_MAX: return fmax32(x, y);
        case VF_MIN: return fmin32(x, y);
        case VF_MAXNM: return fmaxnm32(x, y);
        case VF_MINNM: return fminnm32(x, y);
        case VF_MULX: return fmulx32(x, y);
        case VF_RECPS: return frecps32(x, y);
        case VF_RSQRTS: return frsqrts32(x, y);
        case VF_ABD: return fsub32(x, y) & ~F32_SIGN;
        default: return fcm32(x, y, kind - VF_CMEQ);
        }
    }
    switch (kind) {
    case VF_ADD: return fadd64(a, b);
    case VF_SUB: return fsub64(a, b);
    case VF_MUL: return fmul64(a, b);
    case VF_DIV: return fdiv64(a, b);
    case VF_MAX: return fmax64(a, b);
    case VF_MIN: return fmin64(a, b);
    case VF_MAXNM: return fmaxnm64(a, b);
    case VF_MINNM: return fminnm64(a, b);
    case VF_MULX: return fmulx64(a, b);
    case VF_RECPS: return frecps64(a, b);
    case VF_RSQRTS: return frsqrts64(a, b);
    case VF_ABD: return fsub64(a, b) & ~F64_SIGN;
    default: return fcm64(a, b, kind - VF_CMEQ);
    }
}
/* Lane-wise FP op; `pairwise` adds/maxes adjacent pairs of the concatenation m:n instead. */
DKR_INLINE V128 vfp3(V128 n, V128 m, int kind, int esize, int lanes, int pairwise) {
    V128 r = vzero();
    int dbl = esize == 8;
    for (int k = 0; k < lanes; k++) {
        uint64_t a, b;
        if (pairwise) {
            int i = 2 * k;
            a = i < lanes ? vget(&n, esize, i) : vget(&m, esize, i - lanes);
            b = i + 1 < lanes ? vget(&n, esize, i + 1) : vget(&m, esize, i + 1 - lanes);
        } else {
            a = vget(&n, esize, k);
            b = vget(&m, esize, k);
        }
        vput(&r, esize, k, vf_op(kind, a, b, dbl));
    }
    return r;
}
/* FMLA/FMLS (vector or by element when `elem` >= 0): d + n*m fused; FMLS negates n first. */
DKR_INLINE V128 vfmla(V128 d, V128 n, V128 m, int esize, int lanes, int sub, int elem) {
    V128 r = vzero();
    for (int k = 0; k < lanes; k++) {
        uint64_t x = vget(&n, esize, k), y = vget(&m, esize, elem >= 0 ? elem : k), acc = vget(&d, esize, k);
        if (esize == 4) vput(&r, 4, k, fmadd32((uint32_t)acc, (uint32_t)x ^ (sub ? F32_SIGN : 0), (uint32_t)y));
        else vput(&r, 8, k, fmadd64(acc, x ^ (sub ? F64_SIGN : 0), y));
    }
    return r;
}
/* FP op of each lane of n with one element of m (FMUL/FMULX by element). */
DKR_INLINE V128 vfp_elem(V128 n, V128 m, int kind, int esize, int lanes, int elem) {
    V128 r = vzero();
    uint64_t y = vget(&m, esize, elem);
    for (int k = 0; k < lanes; k++) vput(&r, esize, k, vf_op(kind, vget(&n, esize, k), y, esize == 8));
    return r;
}

enum { VU_RECPE, VU_RSQRTE, VU_ABS, VU_NEG, VU_SQRT, VU_CMGT0, VU_CMEQ0, VU_CMLT0, VU_CMGE0, VU_CMLE0,
       VU_SCVTF, VU_UCVTF, VU_FCVTZS, VU_FCVTZU };
DKR_INLINE V128 vfp2(V128 n, int kind, int esize, int lanes) {
    V128 r = vzero();
    int dbl = esize == 8;
    for (int k = 0; k < lanes; k++) {
        uint64_t a = vget(&n, esize, k), x = 0;
        switch (kind) {
        case VU_RECPE: x = dbl ? frecpe64(a) : frecpe32((uint32_t)a); break;
        case VU_RSQRTE: x = dbl ? frsqrte64(a) : frsqrte32((uint32_t)a); break;
        case VU_ABS: x = a & ~(dbl ? F64_SIGN : F32_SIGN); break;
        case VU_NEG: x = a ^ (dbl ? F64_SIGN : F32_SIGN); break;
        case VU_SQRT: x = dbl ? fsqrt64(a) : fsqrt32((uint32_t)a); break;
        case VU_CMGT0: x = dbl ? fcm64(a, 0, 2) : fcm32((uint32_t)a, 0, 2); break;
        case VU_CMEQ0: x = dbl ? fcm64(a, 0, 0) : fcm32((uint32_t)a, 0, 0); break;
        case VU_CMLT0: x = dbl ? fcm64(a, 0, 4) : fcm32((uint32_t)a, 0, 4); break;
        case VU_CMGE0: x = dbl ? fcm64(a, 0, 1) : fcm32((uint32_t)a, 0, 1); break;
        case VU_CMLE0: x = dbl ? fcm64(a, 0, 3) : fcm32((uint32_t)a, 0, 3); break;
        case VU_SCVTF: x = dbl ? scvtf64((int64_t)a) : scvtf32((int32_t)(uint32_t)a); break;
        case VU_UCVTF: x = dbl ? ucvtf64(a) : ucvtf32((uint32_t)a); break;
        case VU_FCVTZS: x = fcvt_to_int(dbl ? f64_of(a) : (double)f32_of((uint32_t)a), DKR_RZ, 0, dbl ? 64 : 32); break;
        case VU_FCVTZU: x = fcvt_to_int(dbl ? f64_of(a) : (double)f32_of((uint32_t)a), DKR_RZ, 1, dbl ? 64 : 32); break;
        }
        vput(&r, esize, k, x);
    }
    return r;
}

/* ---- integer vector operations ---------------------------------------------------------------------------- */
enum { VI_ADD, VI_SUB, VI_MUL, VI_CMEQ, VI_CMGT, VI_CMGE, VI_CMHI, VI_CMHS, VI_CMTST, VI_ADDP };
DKR_INLINE V128 vint3(V128 n, V128 m, int kind, int esize, int lanes) {
    V128 r = vzero();
    uint64_t mk = emask(esize);
    for (int k = 0; k < lanes; k++) {
        uint64_t a, b, x = 0;
        if (kind == VI_ADDP) {
            int i = 2 * k;
            a = i < lanes ? vget(&n, esize, i) : vget(&m, esize, i - lanes);
            b = i + 1 < lanes ? vget(&n, esize, i + 1) : vget(&m, esize, i + 1 - lanes);
        } else {
            a = vget(&n, esize, k);
            b = vget(&m, esize, k);
        }
        switch (kind) {
        case VI_ADD: case VI_ADDP: x = a + b; break;
        case VI_SUB: x = a - b; break;
        case VI_MUL: x = a * b; break;
        case VI_CMEQ: x = a == b ? mk : 0; break;
        case VI_CMGT: x = sext_e(a, esize) > sext_e(b, esize) ? mk : 0; break;
        case VI_CMGE: x = sext_e(a, esize) >= sext_e(b, esize) ? mk : 0; break;
        case VI_CMHI: x = a > b ? mk : 0; break;
        case VI_CMHS: x = a >= b ? mk : 0; break;
        case VI_CMTST: x = (a & b) ? mk : 0; break;
        }
        vput(&r, esize, k, x & mk);
    }
    return r;
}
/* MLA/MLS: d +/- n*m per element (modulo the element size). */
DKR_INLINE V128 vmla(V128 d, V128 n, V128 m, int esize, int lanes, int sub) {
    V128 r = vzero();
    for (int k = 0; k < lanes; k++) {
        uint64_t p = vget(&n, esize, k) * vget(&m, esize, k), acc = vget(&d, esize, k);
        vput(&r, esize, k, (sub ? acc - p : acc + p) & emask(esize));
    }
    return r;
}
/* Widening ops on the low (or, for the 2 forms, high) halves: [SU]ADDL, [SU]SUBL, [SU]MLAL, [SU]MLSL, [SU]MULL. */
enum { VW_ADDL, VW_SUBL, VW_MLAL, VW_MLSL, VW_MULL };
DKR_INLINE V128 vwiden(V128 d, V128 n, V128 m, int kind, int is_signed, int esize, int upper) {
    V128 r = vzero();
    int lanes = 8 / esize, base = upper ? lanes : 0;
    uint64_t wm = emask(esize * 2);
    for (int k = 0; k < lanes; k++) {
        uint64_t a = vget(&n, esize, base + k), b = vget(&m, esize, base + k);
        if (is_signed) { a = (uint64_t)sext_e(a, esize); b = (uint64_t)sext_e(b, esize); }
        uint64_t acc = vget(&d, esize * 2, k), x;
        switch (kind) {
        case VW_ADDL: x = a + b; break;
        case VW_SUBL: x = a - b; break;
        case VW_MLAL: x = acc + a * b; break;
        case VW_MLSL: x = acc - a * b; break;
        default: x = a * b; break;
        }
        vput(&r, esize * 2, k, x & wm);
    }
    return r;
}

/* BSL/BIT/BIF on whole registers (Q=0: low 64 bits, high bits cleared). */
DKR_INLINE V128 vbitsel(V128 d, V128 n, V128 m, int kind, int q) {
    V128 r = vzero();
    for (int k = 0; k < (q ? 2 : 1); k++) {
        uint64_t D = d.d[k], N = n.d[k], M = m.d[k];
        r.d[k] = kind == 0 ? (N & D) | (M & ~D)     /* BSL: d selects n */
               : kind == 1 ? (N & M) | (D & ~M)     /* BIT: insert n where m is 1 */
               : (N & ~M) | (D & M);                /* BIF: insert n where m is 0 */
    }
    return r;
}
enum { VI2_REV16, VI2_REV32, VI2_REV64, VI2_CNT, VI2_NOT, VI2_CMGT0, VI2_CMEQ0, VI2_CMLT0, VI2_CMGE0, VI2_CMLE0,
       VI2_ABS, VI2_NEG };
DKR_INLINE V128 vint2(V128 n, int kind, int esize, int lanes) {
    V128 r = vzero();
    uint64_t mk = emask(esize);
    if (kind <= VI2_REV64) { /* reverse esize-byte elements inside each container */
        int cont = kind == VI2_REV16 ? 2 : kind == VI2_REV32 ? 4 : 8;
        int bytes = lanes * esize;
        for (int i = 0; i < bytes; i += esize) {
            int base = i - i % cont, j = base + (cont - esize) - (i - base);
            vput(&r, esize, j / esize, vget(&n, esize, i / esize));
        }
        return r;
    }
    for (int k = 0; k < lanes; k++) {
        uint64_t a = vget(&n, esize, k), x = 0;
        int64_t s = sext_e(a, esize);
        switch (kind) {
        case VI2_CNT: for (uint64_t t = a; t; t &= t - 1) x++; break;
        case VI2_NOT: x = ~a; break;
        case VI2_CMGT0: x = s > 0 ? mk : 0; break;
        case VI2_CMEQ0: x = s == 0 ? mk : 0; break;
        case VI2_CMLT0: x = s < 0 ? mk : 0; break;
        case VI2_CMGE0: x = s >= 0 ? mk : 0; break;
        case VI2_CMLE0: x = s <= 0 ? mk : 0; break;
        case VI2_ABS: x = s < 0 ? (uint64_t)-s : (uint64_t)s; break;
        case VI2_NEG: x = (uint64_t)-s; break;
        }
        vput(&r, esize, k, x & mk);
    }
    return r;
}
/* SHL / SSHR / USHR by immediate (right shifts by esize*8 are allowed). */
DKR_INLINE V128 vshift(V128 n, int kind, int shift, int esize, int lanes) {
    V128 r = vzero();
    int bitsz = 8 * esize;
    for (int k = 0; k < lanes; k++) {
        uint64_t a = vget(&n, esize, k), x;
        if (kind == 0) x = a << shift;                                              /* SHL */
        else if (kind == 1) x = (uint64_t)(sext_e(a, esize) >> (shift >= bitsz ? bitsz - 1 : shift)); /* SSHR */
        else x = shift >= bitsz ? 0 : a >> shift;                                   /* USHR */
        vput(&r, esize, k, x & emask(esize));
    }
    return r;
}
/* SSHLL/USHLL(2): widen the low (or, for the 2 form, high) half and shift left. */
DKR_INLINE V128 vshll(V128 n, int is_signed, int shift, int esize, int upper) {
    V128 r = vzero();
    int lanes = 8 / esize, base = upper ? lanes : 0;
    for (int k = 0; k < lanes; k++) {
        uint64_t a = vget(&n, esize, base + k);
        uint64_t x = is_signed ? (uint64_t)sext_e(a, esize) : a;
        vput(&r, esize * 2, k, (x << shift) & emask(esize * 2));
    }
    return r;
}
/* XTN(2): narrow each 2*esize element to esize; the 2 form writes the high half and keeps the low half of d. */
DKR_INLINE V128 vxtn(V128 d, V128 n, int esize, int upper) {
    V128 r = upper ? d : vzero();
    int lanes = 8 / esize, base = upper ? lanes : 0;
    if (!upper) r.d[1] = 0;
    for (int k = 0; k < lanes; k++) vput(&r, esize, base + k, vget(&n, esize * 2, k));
    return r;
}
/* ZIP1/ZIP2/UZP1/UZP2/TRN1/TRN2 (kind = opcode field). */
DKR_INLINE V128 vperm(V128 n, V128 m, int op, int esize, int lanes) {
    V128 r = vzero();
    int half = lanes / 2, part = op >= 5;
    for (int k = 0; k < lanes; k++) {
        uint64_t x = 0;
        switch (op & 3) {
        case 3: { /* ZIP */
            int i = (part ? half : 0) + k / 2;
            x = (k & 1) ? vget(&m, esize, i) : vget(&n, esize, i);
            break;
        }
        case 1: { /* UZP: even (1) or odd (2) elements of the concatenation n:m */
            int i = 2 * k + part;
            x = i < lanes ? vget(&n, esize, i) : vget(&m, esize, i - lanes);
            break;
        }
        case 2: { /* TRN */
            int i = (k & ~1) + part;
            x = (k & 1) ? vget(&m, esize, i) : vget(&n, esize, i);
            break;
        }
        }
        vput(&r, esize, k, x);
    }
    return r;
}
/* EXT: bytes [imm, imm + size) of the concatenation m:n (n low). */
DKR_INLINE V128 vext(V128 n, V128 m, int imm, int q) {
    V128 r = vzero();
    int size = q ? 16 : 8;
    for (int k = 0; k < size; k++) {
        int i = imm + k;
        r.b[k] = i < size ? n.b[i] : m.b[i - size];
    }
    return r;
}
/* TBL/TBX with `nregs` consecutive table registers starting at `first` (numbers wrap at 32). */
DKR_INLINE V128 vtbl(const Cpu *c, V128 d, V128 idx, int first, int nregs, int tbx, int q) {
    V128 r = vzero();
    int size = q ? 16 : 8;
    for (int k = 0; k < size; k++) {
        int i = idx.b[k];
        if (i < 16 * nregs) r.b[k] = c->v[(first + i / 16) % 32].b[i % 16];
        else r.b[k] = tbx ? d.b[k] : 0;
    }
    return r;
}
DKR_INLINE V128 vdup_elem(V128 n, int esize, int index, int lanes) {
    V128 r = vzero();
    uint64_t x = vget(&n, esize, index);
    for (int k = 0; k < lanes; k++) vput(&r, esize, k, x);
    return r;
}
DKR_INLINE V128 vrep64(uint64_t pattern, int q) {
    V128 r;
    r.d[0] = pattern;
    r.d[1] = q ? pattern : 0;
    return r;
}
DKR_INLINE uint64_t vaddv(V128 n, int esize, int lanes) {
    uint64_t s = 0;
    for (int k = 0; k < lanes; k++) s += vget(&n, esize, k);
    return s & emask(esize);
}

#endif
