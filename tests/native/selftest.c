/* Known-answer tests for runtime/include/dkr/cpu.h, built by CMake with whichever C compiler is configured.
 * Expected values follow the Arm ARM pseudocode; the cases are the ones where C or x86 behave differently. */
#include "dkr/rt.h"

#include <stdio.h>

static int failures;
#define CHECK(expr, want)                                                                            \
    do {                                                                                             \
        unsigned long long got_ = (unsigned long long)(expr), want_ = (unsigned long long)(want);     \
        if (got_ != want_) {                                                                         \
            printf("FAIL %s:%d %s = %#llx, want %#llx\n", __FILE__, __LINE__, #expr, got_, want_);   \
            failures++;                                                                              \
        }                                                                                            \
    } while (0)

int main(void) {
    Cpu c = {0};

    /* division never traps */
    CHECK(udiv64(5, 0), 0);
    CHECK(sdiv64(0x8000000000000000ull, ~0ull), 0x8000000000000000ull);
    CHECK(sdiv32(0x80000000u, ~0u), 0x80000000u);
    CHECK(sdiv64((uint64_t)-7, 2), (uint64_t)-3); /* rounds toward zero */
    CHECK(udiv32(7, 0), 0);

    /* 128-bit products */
    CHECK(umulh64(~0ull, ~0ull), 0xFFFFFFFFFFFFFFFEull);
    CHECK(smulh64(~0ull, ~0ull), 0);                       /* -1 * -1 = 1 */
    CHECK(smulh64(0x8000000000000000ull, 2), ~0ull);       /* INT64_MIN * 2 = -2^64 */

    /* shifts and rotates use the amount modulo the width */
    CHECK(ror64(1, 65), 0x8000000000000000ull);
    CHECK(asr32(0x80000000u, 31), 0xFFFFFFFFu);
    CHECK(ror32(0x12345678u, 0), 0x12345678u);

    /* counting and reversing */
    CHECK(clz64(0), 64);
    CHECK(clz32(0), 32);
    CHECK(clz64(1), 63);
    CHECK(cls64(0), 63);
    CHECK(cls64(~0ull), 63);
    CHECK(cls64(1), 62);
    CHECK(cls32(0x40000000u), 0);
    CHECK(rbit32(1), 0x80000000u);
    CHECK(rev_lanes(0x1122334455667788ull, 64), 0x8877665544332211ull);
    CHECK(rev_lanes(0x1122334455667788ull, 32), 0x4433221188776655ull);
    CHECK(rev_lanes(0x1122334455667788ull, 16), 0x2211443366558877ull);

    /* AddWithCarry flags: CMP 0, 1 -> N, borrow (C clear) */
    awc64(&c, 0, ~1ull, 1, 1);
    CHECK(c.nzcv, DKR_N);
    /* CMP x, x -> Z and C */
    awc64(&c, 5, ~5ull, 1, 1);
    CHECK(c.nzcv, DKR_Z | DKR_C);
    /* signed overflow: INT64_MAX + 1 */
    awc64(&c, 0x7FFFFFFFFFFFFFFFull, 1, 0, 1);
    CHECK(c.nzcv, DKR_N | DKR_V);
    /* 32-bit: 0xFFFFFFFF + 1 -> Z and C */
    awc32(&c, 0xFFFFFFFFu, 1, 0, 1);
    CHECK(c.nzcv, DKR_Z | DKR_C);

    /* condition codes on NZCV = Z|C */
    {
        static const int want[16] = {1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1, 1};
        for (int cond = 0; cond < 16; cond++) CHECK(cond_holds(DKR_Z | DKR_C, cond), want[cond]);
    }

    /* SIMD lane helpers */
    CHECK(vrep(0xAB, 1), 0xABABABABABABABABull);
    CHECK(vrep(0x12345678, 4), 0x1234567812345678ull);

    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
