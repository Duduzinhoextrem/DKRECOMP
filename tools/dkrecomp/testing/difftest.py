"""Differential testing: the same A64 code runs in Unicorn (reference) and as recompiled C (tests/diff/harness.c).

Unicorn is used only here, in tests. It is GPL-2.0 software and is never linked into anything DKRecomp ships.
The reference CPU model is Cortex-A72 (ARMv8.0, like the Switch's Cortex-A57): no LSE atomics, no PAC.
"""
from __future__ import annotations

import os
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from typing import List, Optional, Sequence

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
STATE_WORDS = 100  # x0..x30, sp, nzcv, tpidr, tpidrro, pc, v0..v31 (low u64, high u64)
I_SP, I_NZCV, I_TPIDR, I_TPIDRRO, I_PC, I_V = 31, 32, 33, 34, 35, 36
# Test SVC semantics shared with tests/diff/harness.c test_svc_hook: x0 = x0 * 3 + imm, x1 = SVC_TEST_X1.
SVC_TEST_X1 = 0xC0FFEE


@dataclass
class Region:
    addr: int
    data: bytes
    dump: bool = False   # compare contents after each case
    hashed: bool = False  # compare an FNV-1a 64 hash instead of the bytes (large regions)

    @property
    def flags(self) -> int:
        return (1 if self.dump else 0) | (2 if self.hashed else 0)


def fnv1a64(data: bytes) -> bytes:
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return struct.pack("<Q", h)


@dataclass
class Case:
    fn: int                # index into the generated case table
    entry: int             # guest address where Unicorn starts
    state: List[int]       # STATE_WORDS values
    stop_at: Optional[int] = None  # Unicorn: stop when pc reaches this (function mode); None: run 1 instruction


@dataclass
class Result:
    state: List[int]
    dumps: List[bytes] = field(default_factory=list)


def zig_cc() -> List[str]:
    return [sys.executable, "-m", "ziglang", "cc"]


def build_harness(gen_c: str, workdir: str, name: str = "harness", opt: str = "-O1") -> str:
    os.makedirs(workdir, exist_ok=True)
    src = os.path.join(workdir, f"{name}_gen.c")
    with open(src, "w", encoding="utf-8") as f:
        f.write(gen_c)
    exe = os.path.join(workdir, f"{name}.exe")
    # -mfma: FMADD needs a hardware fused multiply-add (libm fma may not be correctly rounded).
    # -ffp-contract=off: no a*b+c contraction the guest did not ask for. -fno-math-errno: plain sqrt.
    cmd = zig_cc() + ["-target", "x86_64-windows-gnu", opt, "-std=c11", "-mfma", "-ffp-contract=off",
                      "-fno-math-errno", "-Wall", "-Wno-unused-label",
                      "-Wno-unused-variable", "-Werror=implicit-function-declaration",
                      "-I", os.path.join(ROOT, "runtime", "include"),
                      os.path.join(ROOT, "tests", "diff", "harness.c"),
                      os.path.join(ROOT, "runtime", "src", "core.c"), src, "-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"harness build failed:\n{r.stderr[-6000:]}")
    return exe


def run_harness(exe: str, regions: Sequence[Region], cases: Sequence[Case], workdir: str,
                timeout: int = 600) -> List[Result]:
    inp, outp = os.path.join(workdir, "cases.bin"), os.path.join(workdir, "results.bin")
    with open(inp, "wb") as f:
        f.write(b"DKRT" + struct.pack("<II", len(regions), len(cases)))
        for r in regions:
            f.write(struct.pack("<QQII", r.addr, len(r.data), r.flags, 0))
            f.write(r.data)
        for c in cases:
            f.write(struct.pack("<II", c.fn, 0) + struct.pack(f"<{STATE_WORDS}Q", *[v & (2**64 - 1) for v in c.state]))
    try:
        r = subprocess.run([exe, inp, outp], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"harness did not finish in {timeout}s (a lifted unit loops?)") from None
    if r.returncode:
        raise RuntimeError(f"harness exited {r.returncode}: {r.stderr[-3000:]}")
    raw = open(outp, "rb").read()
    out, pos = [], 0
    dumps = [8 if x.hashed else len(x.data) for x in regions if x.dump]
    for _ in cases:
        st = list(struct.unpack_from(f"<{STATE_WORDS}Q", raw, pos))
        pos += STATE_WORDS * 8
        ds = []
        for n in dumps:
            ds.append(raw[pos:pos + n])
            pos += n
        out.append(Result(st, ds))
    return out


def run_unicorn(regions: Sequence[Region], cases: Sequence[Case]) -> List[Result]:
    from unicorn import UC_ARCH_ARM64, UC_MODE_ARM, Uc, UcError
    from unicorn import arm64_const as A

    xregs = [getattr(A, f"UC_ARM64_REG_X{i}") for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
    qregs = [getattr(A, f"UC_ARM64_REG_Q{i}") for i in range(32)]
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    try:
        uc.ctl_set_cpu_model(A.UC_CPU_ARM64_A72)
    except Exception:
        pass  # older Unicorn: default model
    for r in regions:
        uc.mem_map(r.addr, (len(r.data) + 0xFFF) & ~0xFFF)
    # Unicorn's exclusive monitor survives from one emu_start to the next, so an LDXR in one case could let a
    # later case's STXR succeed. Each case starts from a fresh CPU in the harness; CLREX gives Unicorn the same.
    clrex_page = 0x7_FFFF_0000
    uc.mem_map(clrex_page, 0x1000)
    uc.mem_write(clrex_page, (0xD503305F).to_bytes(4, "little"))

    def on_intr(uc_, intno, _user):
        # SVC: the same handler as test_svc_hook in tests/diff/harness.c. Unicorn calls this with PC already
        # past the SVC (the exception return address).
        pc = uc_.reg_read(A.UC_ARM64_REG_PC)
        word = int.from_bytes(uc_.mem_read(pc - 4, 4), "little")
        if intno != 2 or word & 0xFFE0001F != 0xD4000001:
            uc_.emu_stop()
            return
        imm = (word >> 5) & 0xFFFF
        x0 = uc_.reg_read(A.UC_ARM64_REG_X0)
        uc_.reg_write(A.UC_ARM64_REG_X0, (x0 * 3 + imm) & (2**64 - 1))
        uc_.reg_write(A.UC_ARM64_REG_X1, SVC_TEST_X1)

    from unicorn import UC_HOOK_INTR
    uc.hook_add(UC_HOOK_INTR, on_intr)
    out = []
    for c in cases:
        uc.emu_start(clrex_page, clrex_page + 4, count=1)
        for r in regions:
            uc.mem_write(r.addr, r.data)
        for i, reg in enumerate(xregs):
            uc.reg_write(reg, c.state[i])
        uc.reg_write(A.UC_ARM64_REG_SP, c.state[I_SP])
        uc.reg_write(A.UC_ARM64_REG_NZCV, c.state[I_NZCV])
        uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0, c.state[I_TPIDR])
        uc.reg_write(A.UC_ARM64_REG_TPIDRRO_EL0, c.state[I_TPIDRRO])
        for k, reg in enumerate(qregs):
            uc.reg_write(reg, c.state[I_V + 2 * k] | (c.state[I_V + 2 * k + 1] << 64))
        err = None
        try:
            if c.stop_at is None:
                uc.emu_start(c.entry, 0xFFFFFFFFFFFFFFFF, count=1)
            else:
                uc.emu_start(c.entry, c.stop_at, count=10_000_000)
        except UcError as e:
            # A taken branch to an unmapped address faults on the *next* fetch, after the branch itself has
            # completed: the registers then hold the branch's result, which is what we compare.
            pc_now = uc.reg_read(A.UC_ARM64_REG_PC)
            if not (c.stop_at is None and "FETCH_UNMAPPED" in str(e) and pc_now != c.entry):
                err = str(e)
        st = [uc.reg_read(r) for r in xregs] + [uc.reg_read(A.UC_ARM64_REG_SP), uc.reg_read(A.UC_ARM64_REG_NZCV),
                                               uc.reg_read(A.UC_ARM64_REG_TPIDR_EL0),
                                               uc.reg_read(A.UC_ARM64_REG_TPIDRRO_EL0), uc.reg_read(A.UC_ARM64_REG_PC)]
        for reg in qregs:
            q = uc.reg_read(reg)
            st += [q & (2**64 - 1), q >> 64]
        dumps = []
        for r in regions:
            if r.dump:
                m = bytes(uc.mem_read(r.addr, len(r.data)))
                dumps.append(fnv1a64(m) if r.hashed else m)
        res = Result(st, dumps)
        res.error = err  # type: ignore[attr-defined]
        out.append(res)
    return out


REG_NAMES = [f"x{i}" for i in range(31)] + ["sp", "nzcv", "tpidr", "tpidrro", "pc"] + \
    [f"v{k}.{half}" for k in range(32) for half in ("lo", "hi")]


def diff_states(ref: Result, got: Result, skip: Sequence[int] = ()) -> List[str]:
    msgs = []
    for i, (a, b) in enumerate(zip(ref.state, got.state)):
        if i in skip:
            continue
        if a != b:
            msgs.append(f"{REG_NAMES[i]}: ref {a:#x} got {b:#x}")
    for k, (a, b) in enumerate(zip(ref.dumps, got.dumps)):
        if a != b:
            first = next(j for j in range(len(a)) if a[j] != b[j])
            msgs.append(f"memory region {k} differs first at +{first:#x}: ref {a[first:first+8].hex()} "
                        f"got {b[first:first+8].hex()}")
    return msgs
