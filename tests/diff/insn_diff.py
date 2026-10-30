"""Single-instruction differential test: every generated instruction runs in Unicorn and as lifted C.

    .venv\\Scripts\\python tests\\diff\\insn_diff.py [--seed N] [--count N] [--states N]

Exit code 0 only when every case matches. A summary per operation is printed; failures list the instruction,
its decoded form and the differing registers or memory.
"""
from __future__ import annotations

import argparse
import collections
import os
import random
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dkrecomp.recomp.lifter import C_PRELUDE, Function, fname, lift_function  # noqa: E402
from dkrecomp.testing import difftest as D  # noqa: E402
from dkrecomp.testing import insn_cases as IC  # noqa: E402


def run(seed: int, count: int, states: int, workdir: str, verbose: bool = True, only: str = ""):
    t0 = time.time()
    insns, cases = IC.generate(seed, count, states, only)
    code = b"".join(i.word.to_bytes(4, "little") for i in insns)
    rng = random.Random(seed ^ 0x5EED)
    below = bytes(rng.getrandbits(8) for _ in range(IC.CODEZONE_BELOW))
    above_len = ((len(code) + IC.CODEZONE_BELOW + 0xFFF) & ~0xFFF)
    above = code + bytes(rng.getrandbits(8) for _ in range(above_len - len(code)))
    scratch = bytes(rng.getrandbits(8) for _ in range(IC.SCRATCH_SIZE))
    regions = [D.Region(IC.CODE - IC.CODEZONE_BELOW, below + above),
               D.Region(IC.SCRATCH, scratch, dump=True)]

    parts = [C_PRELUDE]
    for i in insns:
        lines, _ = lift_function(code, IC.CODE, Function(i.addr, i.addr + 4), exit_mode="pc")
        parts += lines
    parts.append("GuestFn dkr_case_table[] = {" + ", ".join(fname(i.addr) for i in insns) + "};")
    parts.append(f"const size_t dkr_case_count = {len(insns)};")
    parts.append('#include "dkr/rt.h"\nconst DkrFnEntry dkr_fn_table[1] = {{0, 0}};\nconst size_t dkr_fn_count = 0;')
    exe = D.build_harness("\n".join(parts) + "\n", workdir, "insn")
    t_build = time.time() - t0

    dcases = [D.Case(k, insns[k].addr, st) for k, st in cases]
    got = D.run_harness(exe, regions, dcases, workdir)
    ref = D.run_unicorn(regions, dcases)

    per_op = collections.Counter()
    bad_op = collections.Counter()
    failures = []
    for c, r, g in zip(dcases, ref, got):
        i = insns[c.fn]
        per_op[i.op] += 1
        err = getattr(r, "error", None)
        msgs = [f"unicorn raised: {err}"] if err else D.diff_states(r, g)
        if msgs:
            bad_op[i.op] += 1
            failures.append((i, c.state, msgs))
    if verbose:
        print(f"seed {seed}: {len(insns)} instructions, {len(dcases)} cases, build {t_build:.1f}s, "
              f"total {time.time() - t0:.1f}s")
        for op in sorted(per_op):
            print(f"  {op:12s} {per_op[op]:6d} cases  {bad_op[op]:4d} failed")
        for i, st, msgs in failures[:25]:
            print(f"FAIL {i!r}")
            for m in msgs[:6]:
                print(f"     {m}")
    return per_op, failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=3000)
    ap.add_argument("--states", type=int, default=3)
    ap.add_argument("--workdir", default=os.path.join(ROOT, "build", "tests", "insn_diff"))
    ap.add_argument("--only", default="", help="template class prefix, e.g. fp_")
    a = ap.parse_args()
    _, failures = run(a.seed, a.count, a.states, a.workdir, only=a.only)
    print("RESULT:", "PASS" if not failures else f"FAIL ({len(failures)} cases)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
