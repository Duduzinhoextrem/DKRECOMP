"""Whole-function differential test (MVP 3).

    .venv\\Scripts\\python tests\\diff\\func_diff.py [--opt O2] [--seeds N]

1. tests/diff/cases/programs.c is compiled by Clang (Zig's) to an AArch64 ELF: the guest program.
2. DKRecomp lifts every function of that ELF to C, which is compiled for x86-64 with the runtime.
3. test_entry(which, seed) runs three ways and must agree:
     Unicorn running the AArch64 code  ==  the recompiled code  ==  programs.c compiled natively for x86-64.
   Recompiled and Unicorn must also agree on every register, SP, NZCV and the program's .data/.bss.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from dkrecomp.formats import elf as E  # noqa: E402
from dkrecomp.recomp.lifter import Function, fname  # noqa: E402
from dkrecomp.recomp.module import lift_module  # noqa: E402
from dkrecomp.testing import difftest as D  # noqa: E402

PROGRAM = os.path.join(ROOT, "tests", "diff", "cases", "programs.c")
BASE = 0x4_0000_0000
STACK = 0x6_0000_0000
STACK_SIZE = 0x40000
RET_MAGIC = STACK  # bottom of the stack region: mapped, never executed, Unicorn stops when pc reaches it


def build_guest(workdir: str, opt: str, vectorize: bool = False) -> str:
    out = os.path.join(workdir, f"programs_{opt}{'_vec' if vectorize else ''}.elf")
    # By default Clang's auto-vectoriser is off; --vectorize turns it on, so the guest has compiler-made SIMD.
    # -ffp-contract=off matches the native build's roundings (no FMADD fusion on one side only).
    # -g keeps the symbol table, which Zig strips at -Os; it does not change the code.
    cmd = D.zig_cc() + ["-target", "aarch64-freestanding-none", f"-{opt}", "-g", "-ffreestanding", "-fno-builtin",
                        *([] if vectorize else ["-fno-vectorize", "-fno-slp-vectorize"]), "-ffp-contract=off", "-fno-math-errno", "-nostdlib",
                        "-fno-stack-protector", "-fno-sanitize=undefined",
                        f"-Wl,--image-base={BASE:#x}", "-Wl,-e,_start", PROGRAM, "-o", out]
    subprocess.run(cmd, check=True)
    return out


def build_native(workdir: str, opt: str) -> str:
    out = os.path.join(workdir, f"native_{opt}.exe")
    cmd = D.zig_cc() + ["-target", "x86_64-windows-gnu", f"-{opt}", "-fno-builtin", "-fno-sanitize=undefined",
                        "-ffp-contract=off", PROGRAM,
                        os.path.join(ROOT, "tests", "diff", "cases", "native_main.c"), "-o", out]
    subprocess.run(cmd, check=True)
    return out


def regions_for(elf: E.Elf):
    regs = []
    for s in elf.segments:
        start = s.vaddr & ~0xFFF
        end = (s.vaddr + len(s.data) + 0xFFF) & ~0xFFF
        buf = bytearray(end - start)
        buf[s.vaddr - start:s.vaddr - start + len(s.data)] = s.data
        regs.append(D.Region(start, bytes(buf), dump=bool(s.flags & 2)))  # compare writable segments
    regs.append(D.Region(STACK, bytes(STACK_SIZE)))
    return regs


def run(opt: str, nseeds: int, workdir: str, vectorize: bool = False) -> int:
    os.makedirs(workdir, exist_ok=True)
    t0 = time.time()
    elf = E.parse(open(build_guest(workdir, opt, vectorize), "rb").read())
    text = next(s for s in elf.segments if s.flags & 1)
    funcs = [Function(f.value, f.value + f.size, f.name) for f in elf.functions()
             if text.vaddr <= f.value < text.vaddr + len(text.data)]
    mod = lift_module(text.data, text.vaddr, funcs)
    entry = next(f.entry for f in funcs if f.name == "test_entry")
    gen = mod.c_source + f"GuestFn dkr_case_table[] = {{{fname(entry)}}};\nconst size_t dkr_case_count = 1;\n"
    with open(os.path.join(workdir, f"lifted_{opt}.c"), "w") as fh:
        fh.write(gen)
    exe = D.build_harness(gen, workdir, f"func_{opt}", opt="-O2")
    native = build_native(workdir, opt)
    print(f"[{opt}] {len(funcs)} functions lifted, {sum(mod.unsupported.values())} unsupported instruction sites "
          f"{mod.unsupported or ''}; build {time.time() - t0:.1f}s")

    pairs = [(w, s) for w in range(13) for s in [0x1234 + 977 * k for k in range(nseeds)]]
    regions = regions_for(elf)
    cases = []
    for w, s in pairs:
        st = [0] * D.STATE_WORDS
        st[0], st[1] = w, s
        st[30] = RET_MAGIC
        st[D.I_SP] = STACK + STACK_SIZE - 0x100
        cases.append(D.Case(0, entry, st, stop_at=RET_MAGIC))
    got = D.run_harness(exe, regions, cases, workdir)
    ref = D.run_unicorn(regions, cases)
    nat_out = subprocess.run([native] + [str(v) for p in pairs for v in p], capture_output=True, text=True,
                             check=True).stdout.split()
    failures = 0
    names = ["fib", "sum_mix", "insertion_sort", "byte_ops", "dispatch_switch", "wide_math", "fold_ptrs",
             "vec_add", "atomics", "clamp_sum", "count_runs", "svc", "fp_physics"]
    per_test = {}
    for (w, s), c, r, g, n in zip(pairs, cases, ref, got, nat_out):
        msgs = []
        if getattr(r, "error", None):
            msgs.append(f"unicorn raised: {r.error}")
        msgs += D.diff_states(r, g, skip=[D.I_PC])
        if g.state[0] != int(n):
            msgs.append(f"x0 recompiled {g.state[0]:#x} != native {int(n):#x}")
        per_test.setdefault(names[w], [0, 0])
        per_test[names[w]][0] += 1
        if msgs:
            failures += 1
            per_test[names[w]][1] += 1
            print(f"FAIL {names[w]}(seed={s:#x}):")
            for m in msgs[:6]:
                print("    " + m)
    for k, (n, bad) in per_test.items():
        print(f"  {k:16s} {n:3d} runs  {bad:3d} failed")
    print(f"[{opt}] {len(cases)} runs, {failures} failed, total {time.time() - t0:.1f}s")
    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--opt", default="O2,O1,O0,Os")
    ap.add_argument("--seeds", type=int, default=8)
    ap.add_argument("--workdir", default=os.path.join(ROOT, "build", "tests", "func"))
    ap.add_argument("--vectorize", action="store_true", help="let Clang auto-vectorise the guest program")
    a = ap.parse_args()
    total = sum(run(o, a.seeds, a.workdir, a.vectorize) for o in a.opt.split(","))
    print("RESULT:", "PASS" if total == 0 else f"FAIL ({total} runs)")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
