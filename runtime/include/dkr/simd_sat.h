/* Saturating integer SIMD operations. Included only by generated functions that need them. */
#ifndef DKR_SIMD_SAT_H
#define DKR_SIMD_SAT_H
#include "dkr/cpu.h"

/* SQADD/UQADD: overflow detection uses unsigned arithmetic even for signed 64-bit lanes. */
DKR_INLINE V128 vqadd(Cpu *c, V128 n, V128 m, int esize, int lanes, int unsign) {
    V128 result = vzero();
    uint64_t mask = emask(esize), sign = 1ull << (esize * 8 - 1);
    for (int i = 0; i < lanes; i++) {
        uint64_t a = vget(&n, esize, i), b = vget(&m, esize, i), sum = (a + b) & mask;
        if (unsign ? b > mask - a : ((~(a ^ b) & (a ^ sum) & sign) != 0)) {
            sum = unsign ? mask : (a & sign) ? sign : sign - 1;
            c->fpsr |= 1ull << 27;
        }
        vput(&result, esize, i, sum);
    }
    return result;
}

/* SQSHRN/SQRSHRN/UQSHRN/UQRSHRN (and 2). Round by testing the discarded half bit,
 * avoiding signed overflow at INT64_MAX and unsigned overflow at UINT64_MAX. FPSR.QC is sticky. */
DKR_INLINE V128 vqshrn(Cpu *c, V128 d, V128 n, int esize, int shift, int upper, int rounding, int unsign) {
    V128 result = upper ? d : vzero();
    int lanes = 8 / esize;
    for (int i = 0; i < lanes; i++) {
        uint64_t bits = vget(&n, esize * 2, i), narrowed;
        unsigned round_bit = rounding ? (unsigned)((bits >> (shift - 1)) & 1) : 0;
        if (unsign) {
            uint64_t value = (bits >> shift) + round_bit, limit = emask(esize);
            if (value > limit) { value = limit; c->fpsr |= 1ull << 27; }
            narrowed = value;
        } else {
            int64_t value = (sext_e(bits, esize * 2) >> shift) + round_bit;
            int64_t min = -(1ll << (esize * 8 - 1)), max = (1ll << (esize * 8 - 1)) - 1;
            if (value < min) { value = min; c->fpsr |= 1ull << 27; }
            if (value > max) { value = max; c->fpsr |= 1ull << 27; }
            narrowed = (uint64_t)value;
        }
        vput(&result, esize, (upper ? lanes : 0) + i, narrowed);
    }
    return result;
}
#endif
