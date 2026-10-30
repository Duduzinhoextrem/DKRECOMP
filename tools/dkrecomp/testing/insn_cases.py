"""Random single-instruction test cases for the differential test.

Words are drawn from encoding-class templates (fixed bits from the Arm ARM encoding tables, the rest random),
decoded by DKRecomp's decoder, and kept when the lifter supports them. Register states mix random and edge
values; memory operands are steered into a scratch page so loads and stores are comparable.
"""
from __future__ import annotations

import random
from typing import Dict, List, Optional, Tuple

from ..arm64.decoder import SP, ZR, Insn, decode
from .difftest import I_NZCV, I_PC, I_SP, I_TPIDR, I_TPIDRRO, I_V, STATE_WORDS

CODE = 0x3_0000_0000
CODEZONE_BELOW = 0x10_0000   # LDR literal reaches +-1 MiB
SCRATCH = 0x2_0000_0000
SCRATCH_SIZE = 0x1000
SCRATCH_MID = SCRATCH + 0x800

# (mask, value) of the fixed bits of each encoding class.
TEMPLATES: Dict[str, Tuple[int, int]] = {
    "adr": (0x1F000000, 0x10000000),
    "addsub_imm": (0x1F800000, 0x11000000),
    "logic_imm": (0x1F800000, 0x12000000),
    "movewide": (0x1F800000, 0x12800000),
    "bitfield": (0x1F800000, 0x13000000),
    "extr": (0x1F800000, 0x13800000),
    "b_bl": (0x7C000000, 0x14000000),
    "cbz": (0x7E000000, 0x34000000),
    "tbz": (0x7E000000, 0x36000000),
    "bcond": (0xFF000010, 0x54000000),
    "ldst_uimm": (0x3F000000, 0x39000000),
    "ldst_imm9": (0x3F200000, 0x38000000),
    "ldst_reg": (0x3F200C00, 0x38200800),
    "ldst_pair": (0x3C000000, 0x28000000),
    "ldr_lit": (0x3F000000, 0x18000000),
    "excl": (0x3F000000, 0x08000000),
    "ldar": (0x3FFFFC00, 0x08DFFC00),
    "stlr": (0x3FFFFC00, 0x089FFC00),
    "ldxr": (0x3FFF7C00, 0x085F7C00),
    "stxr": (0x3FE07C00, 0x08007C00),
    "logic_reg": (0x1F000000, 0x0A000000),
    "addsub_reg": (0x1F000000, 0x0B000000),
    "dp3": (0x7F000000, 0x1B000000),
    "adc": (0x1FE0FC00, 0x1A000000),
    "ccmp": (0x1FE00410, 0x1A400000),
    "csel": (0x1FE00800, 0x1A800000),
    "dp2": (0x5FE0C000, 0x1AC00000),   # opcode bits 15:14 = 0: UDIV, SDIV, LSLV..RORV and neighbours
    "dp1": (0x7FFFE000, 0x5AC00000),   # bits 20:13 = 0: RBIT..CLS
    # SIMD&FP register moves and memory
    "v_ldst_uimm": (0x3F000000, 0x3D000000),
    "v_ldst_imm9": (0x3F200000, 0x3C000000),
    "v_ldst_reg": (0x3F200C00, 0x3C200800),
    "v_ldst_pair": (0x3C000000, 0x2C000000),
    "v_ldr_lit": (0x3F000000, 0x1C000000),
    "fmov_gp": (0x7F36FC00, 0x1E260000),      # sf/ftype/rmode/opcode<0> random: FMOV forms and neighbours
    "simd_copy": (0xBFE08400, 0x0E000400),
    "dup_scalar": (0xFFE0FC00, 0x5E000400),
    "vlogic": (0x9F20FC00, 0x0E201C00),
    "movi64": (0xBFF8FC00, 0x2F00E400),
    # scalar floating point (ftype bit 23 fixed to 0: single or double)
    "fp_dp1": (0xFFA07C00, 0x1E204000),
    "fp_dp2": (0xFFA00C00, 0x1E200800),
    "fp_dp3": (0xFF800000, 0x1F000000),
    "fp_cmp": (0xFFA0FC07, 0x1E202000),
    "fp_ccmp": (0xFFA00C00, 0x1E200400),
    "fp_csel": (0xFFA00C00, 0x1E200C00),
    "fp_imm": (0xFFA01FE0, 0x1E201000),
    "fp_int": (0x7FA0FC00, 0x1E200000),
    "fp_int_v": (0xDFBFFC00, 0x5E21D800),
    "fp_fixed": (0x7FA00000, 0x1E000000),
}

# Floating-point bit patterns that exercise the A64/x86 differences (fp.h): zeros, infinities, quiet and
# signalling NaNs with payloads, subnormals, rounding ties, integer-conversion limits.
FP32_EDGE = [0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7FA00001, 0xFF812345,
             0x7FC12345, 0xFFD00001, 0x00000001, 0x807FFFFF, 0x00800000, 0x3F800000, 0xBF800000, 0x3F000000,
             0xBF000000, 0x3FC00000, 0xBFC00000, 0x40200000, 0xC0200000, 0x4F000000, 0xCF000000, 0x4F800000,
             0x5F000000, 0xDF000000, 0x5F800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x4B000001, 0x4B7FFFFF, 0x3EAAAAAB,
             0x4EFFFFFF, 0xCF000001]
FP64_EDGE = [0x0000000000000000, 0x8000000000000000, 0x7FF0000000000000, 0xFFF0000000000000,
             0x7FF8000000000000, 0xFFF8000000000000, 0x7FF4000000000001, 0xFFF0000000012345,
             0x7FF8000012345678, 0x0000000000000001, 0x800FFFFFFFFFFFFF, 0x0010000000000000,
             0x3FF0000000000000, 0xBFF0000000000000, 0x3FE0000000000000, 0x3FF8000000000000,
             0xBFF8000000000000, 0x4004000000000000, 0x41E0000000000000, 0xC1E0000000000000,
             0x41DFFFFFFFC00000, 0x41F0000000000000, 0x43E0000000000000, 0xC3E0000000000000,
             0x43F0000000000000, 0x7FEFFFFFFFFFFFFF, 0x4330000000000001, 0x3FD5555555555555,
             0x36A0000000000000, 0x47F0000000000000, 0x380FFFFFE0000000]


def random_fp64(rng: random.Random) -> int:
    k = rng.random()
    if k < 0.35:
        return rng.choice(FP64_EDGE)
    if k < 0.7:  # ordinary numbers of moderate magnitude, so arithmetic results are mostly normal
        return (rng.getrandbits(1) << 63) | (rng.randrange(0x3C0, 0x440) << 52) | rng.getrandbits(52)
    return rng.getrandbits(64)


def random_fp32(rng: random.Random) -> int:
    k = rng.random()
    if k < 0.35:
        return rng.choice(FP32_EDGE)
    if k < 0.7:
        return (rng.getrandbits(1) << 31) | (rng.randrange(0x60, 0xA0) << 23) | rng.getrandbits(23)
    return rng.getrandbits(32)
FIXED = {
    "br": [0xD61F0000], "blr": [0xD63F0000], "ret": [0xD65F0000],
    "mrs": [0xD53B4200, 0xD53BD040, 0xD53BD060],   # nzcv, tpidr_el0, tpidrro_el0
    "msr": [0xD51B4200, 0xD51BD040],                # nzcv, tpidr_el0
    "nop": [0xD503201F], "dmb": [0xD5033BBF],
}

# Ops the random test cannot compare meaningfully against Unicorn.
SKIP_OPS = {"unsupported", "svc", "brk"}
SKIP_SYSREGS = {"cntvct_el0", "cntfrq_el0", "fpcr", "fpsr", "dczid_el0"}

EDGE = [0, 1, 2, 0x7F, 0x80, 0xFF, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000,
        0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF80000000]


def random_word(rng: random.Random, cls: str) -> int:
    if cls in FIXED:
        w = rng.choice(FIXED[cls])
        if cls in ("br", "blr", "ret"):
            return w | (rng.randrange(31) << 5)
        if cls in ("mrs", "msr"):
            return w | rng.randrange(31)
        return w
    mask, val = TEMPLATES[cls]
    return (rng.getrandbits(32) & ~mask) | val


def acceptable(i: Insn) -> bool:
    if i.op in SKIP_OPS:
        return False
    if i.op in ("mrs", "msr") and i.sysreg in SKIP_SYSREGS:
        return False
    if i.op == "nop" and i.word != 0xD503201F:
        return False  # other hints (PAC, BTI...) are not used on ARMv8.0 and Unicorn may model them
    f = i.f
    # CONSTRAINED UNPREDICTABLE register overlaps: the architecture allows several outcomes.
    if i.op in ("ld", "st") and f.get("wb") and f["rt"] == f["rn"] and f["rn"] != SP:
        return False
    if i.op == "vldp" and f["rt"] == f["rt2"]:
        return False
    if i.op in ("ldp", "stp"):
        if i.op == "ldp" and f["rt"] == f["rt2"]:
            return False
        if f["wb"] and f["rn"] != SP and f["rn"] in (f["rt"], f["rt2"]):
            return False
    if i.op == "stxr" and (f["rs"] == f["rt"] or f["rs"] == f["rn"]):
        return False
    return True


def random_value(rng: random.Random) -> int:
    k = rng.random()
    if k < 0.3:
        return rng.choice(EDGE)
    if k < 0.5:
        return rng.getrandbits(32)
    if k < 0.6:
        return rng.randrange(64)
    return rng.getrandbits(64)


def make_state(rng: random.Random, i: Insn, pc: int) -> Optional[List[int]]:
    s = [random_value(rng) for _ in range(STATE_WORDS)]
    s[I_SP] = SCRATCH_MID & ~0xF
    s[I_NZCV] = rng.randrange(16) << 28
    s[I_TPIDR] = rng.getrandbits(64)
    s[I_TPIDRRO] = rng.getrandbits(64)
    s[I_PC] = pc
    for k in range(64):
        s[I_V + k] = rng.getrandbits(64)
    if i.op in ("fp1", "fp2", "fp3", "fcvt", "fcmp", "fccmp", "fcsel", "fcvt_int", "int_to_fp_v"):
        for r in range(32):  # low lane holds an interesting single or double; the rest stays random
            if i.f.get("n", 4) == 4 and rng.random() < 0.5:
                s[I_V + 2 * r] = (s[I_V + 2 * r] & ~0xFFFFFFFF) | random_fp32(rng)
            else:
                s[I_V + 2 * r] = random_fp64(rng) if i.f.get("n") == 8 else \
                    (s[I_V + 2 * r] & ~0xFFFFFFFF) | random_fp32(rng)
        # Related operands make the interesting cases common: x-x, inf-inf, 0*inf, equal compares, -0 results.
        if rng.random() < 0.4 and "rn" in i.f:
            src = s[I_V + 2 * i.f["rn"]]
            flip = (1 << (63 if i.f.get("n") == 8 else 31)) if rng.random() < 0.5 else 0
            for key in ("rm", "ra"):
                if key in i.f and rng.random() < 0.7:
                    s[I_V + 2 * i.f[key]] = src ^ flip
        if rng.random() < 0.2 and "rm" in i.f and "rn" in i.f:  # inf against zero for FMUL/FMADD
            inf, zero = (0x7FF0000000000000, 0) if i.f.get("n") == 8 else (0x7F800000, 0)
            s[I_V + 2 * i.f["rn"]] = inf | (rng.getrandbits(1) << (63 if i.f.get("n") == 8 else 31))
            s[I_V + 2 * i.f["rm"]] = zero | (rng.getrandbits(1) << (63 if i.f.get("n") == 8 else 31))
    f = i.f
    mem_ops = ("ld", "st", "ld_reg", "st_reg", "ldp", "stp", "ldar", "stlr", "ldxr", "stxr",
               "vld", "vst", "vld_reg", "vst_reg", "vldp", "vstp")
    if i.op in mem_ops:
        align = f["nbytes"] if i.op in ("ldar", "stlr", "ldxr", "stxr") else 1
        target = SCRATCH_MID + (rng.randrange(-0x200, 0x200) & ~(align - 1) if align > 1 else
                                rng.randrange(-0x200, 0x200))
        if i.op in ("ld_reg", "st_reg", "vld_reg", "vst_reg"):
            rm, option = f["rm"], f["option"]
            if rm == f["rn"]:
                return None  # index and base are one register; cannot steer the address into scratch
            idx = 0
            if rm != ZR:
                small = rng.randrange(0x40) if option == 2 else rng.randrange(-0x20, 0x20)
                if option in (2, 6):  # only the low 32 bits are used; randomise the upper half
                    s[rm] = (rng.getrandbits(32) << 32) | (small & 0xFFFFFFFF)
                else:
                    s[rm] = small & (2**64 - 1)
                idx = small
            base = target - (idx << f["shift"])
        else:
            imm = 0 if f.get("wb") == "post" else f.get("imm", 0)
            base = target - imm
        if f["rn"] == SP:
            if base & 0xF:
                return None  # keep SP 16-byte aligned (SP alignment checks)
            s[I_SP] = base
        else:
            s[f["rn"]] = base
    return s


def generate(seed: int, count: int, states_per_insn: int = 2, only: str = ""):
    """Returns (instructions, cases) where cases is a list of (insn index, state). `only` keeps the template
    classes whose name starts with that prefix (e.g. "fp_")."""
    rng = random.Random(seed)
    classes = [c for c in list(TEMPLATES) + list(FIXED) if c.startswith(only)]
    insns: List[Insn] = []
    cases: List[Tuple[int, List[int]]] = []
    attempts = 0
    while len(insns) < count and attempts < count * 200:
        attempts += 1
        cls = classes[len(insns) % len(classes)] if rng.random() < 0.7 else rng.choice(classes)
        w = random_word(rng, cls)
        pc = CODE + 4 * len(insns)
        i = decode(w, pc)
        if not acceptable(i):
            continue
        states = []
        for _ in range(states_per_insn * 4):
            st = make_state(rng, i, pc)
            if st is not None:
                states.append(st)
            if len(states) == states_per_insn:
                break
        if not states:
            continue
        k = len(insns)
        insns.append(i)
        cases += [(k, st) for st in states]
    return insns, cases
