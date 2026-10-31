"""Every vector saturating narrowing shift, including FPSR.QC, checked against Unicorn.

The four-instruction functions set FPSR, narrow/round, read FPSR into X0 and return.
No copyrighted input is used. All element sizes, immediates, upper forms and aliases are covered.
"""
import os
import random
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from dkrecomp.recomp.lifter import C_PRELUDE, Function, fname, lift_function
from dkrecomp.testing import difftest as D


def run(workdir):
    base = 0x300000000
    rng = random.Random(0x514e)
    functions = []
    for esize in (1, 2, 4):
        for shift in range(1, esize * 8 + 1):
            for flags in range(8):  # upper, unsigned, rounding
                for rd in (0, 1):  # source/destination alias and distinct registers
                    word = (0x0f009400 | ((flags & 1) << 30) | (((flags >> 1) & 1) << 29) |
                            (((flags >> 2) & 1) << 11) | ((16 * esize - shift) << 16) | rd)
                    functions.append((word, esize * 16, 1 << (esize * 8 - 1 + shift), shift, False))
    for esize in (1, 2, 4, 8):
        for q in (0, 1):
            if esize == 8 and not q:
                continue
            for unsigned in (0, 1):
                for rd in (0, 1):
                    word = 0x0e220c00 | (q << 30) | (unsigned << 29) | ((esize.bit_length() - 1) << 22) | rd
                    functions.append((word, esize * 8, 1 << (esize * 8 - 1), 1, True))
    code = b''.join(struct.pack('<4I', 0xd51b4421, word, 0xd53b4420, 0xd65f03c0)
                    for word, *_ in functions)
    end = base + len(code) + 4
    parts, cases = [C_PRELUDE], []
    for k, (word, width, threshold, shift, is_add) in enumerate(functions):
        entry = base + k * 16
        lines, unsupported = lift_function(code, base, Function(entry, entry + 16), exit_mode='pc')
        assert not unsupported, hex(word)
        parts += lines
        mask = (1 << width) - 1
        values = [0, 1, mask, mask >> 1, 1 << (width - 1), threshold - 1, threshold,
                  threshold + 1, (1 << shift) - 1, 1 << (shift - 1)]
        for variant in range(16):
            state = [0] * D.STATE_WORDS
            state[30] = end
            state[1] = (variant & 1) << 27  # exercise clear and already-set QC
            state[D.I_SP] = 0x200000000
            for j in range(D.I_V, D.STATE_WORDS):
                state[j] = rng.getrandbits(64)
            lanes = [values[(variant + j) % len(values)] if variant < 10 else rng.getrandbits(width)
                     for j in range(128 // width)]
            vector = sum((value & mask) << (width * j) for j, value in enumerate(lanes))
            state[D.I_V] = vector & ((1 << 64) - 1)
            state[D.I_V + 1] = vector >> 64
            if is_add:
                if variant < 2:
                    edge = (1 << (width - 1)) - 1 + variant
                    vector = sum(edge << (width * j) for j in range(128 // width))
                    state[D.I_V] = state[D.I_V + 4] = vector & ((1 << 64) - 1)
                    state[D.I_V + 1] = state[D.I_V + 5] = vector >> 64
                elif variant < 10:
                    state[D.I_V + 4] = state[D.I_V]
                    state[D.I_V + 5] = state[D.I_V + 1]
            cases.append(D.Case(k, entry, state, stop_at=end))
    parts += ['GuestFn dkr_case_table[] = {' + ','.join(fname(base + k * 16) for k in range(len(functions))) + '};',
              f'const size_t dkr_case_count = {len(functions)};',
              '#include "dkr/rt.h"\nconst DkrFnEntry dkr_fn_table[1] = {{0}};\nconst size_t dkr_fn_count = 0;']
    executable = D.build_harness('\n'.join(parts), workdir, 'saturating')
    regions = [D.Region(base, code + b'\0' * 16)]
    actual = D.run_harness(executable, regions, cases, workdir)
    expected = D.run_unicorn(regions, cases)
    failures = []
    for case, ref, got in zip(cases, expected, actual):
        differences = D.diff_states(ref, got)
        if ref.error or differences:
            failures.append((hex(functions[case.fn][0]), ref.error, differences[:3]))
    print(f'Saturating narrowing shifts/add: {len(functions)} encodings, {len(cases)} cases, {len(failures)} differences')
    return failures


if __name__ == '__main__':
    failures = run(os.path.join(ROOT, 'build', 'tests', 'saturating'))
    for failure in failures[:10]:
        print(failure)
    sys.exit(bool(failures))
