"""A64 decoder for the integer subset DKRecomp lifts.

Written for DKRecomp from the Arm Architecture Reference Manual (DDI 0487), section C4 "A64 Instruction Set
Encoding". Each `decode()` result is an `Insn` whose `op` names an operation the lifter knows; any encoding outside
the subset returns op='unsupported' with a reason, so coverage gaps are visible instead of silently wrong.

Register numbering: 0-30 are X/W registers. Register 31 is either SP or ZR depending on the operand; the decoder
resolves that and stores it as `SP` (32) or `ZR` (31), so the lifter never guesses.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, Optional

ZR = 31
SP = 32

COND_NAMES = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"]


@dataclass
class Insn:
    addr: int
    word: int
    op: str
    f: Dict[str, Any] = field(default_factory=dict)

    def __getattr__(self, k):  # convenient field access: insn.rd (field names must not be addr/word/op/f)
        try:
            return self.__dict__["f"][k]
        except KeyError:
            raise AttributeError(k) from None

    def __repr__(self):
        return f"Insn({self.addr:#x}: {self.word:08x} {self.op} {self.f})"


def bits(w: int, lo: int, n: int) -> int:
    return (w >> lo) & ((1 << n) - 1)


def sext(v: int, n: int) -> int:
    return v - (1 << n) if v & (1 << (n - 1)) else v


def _ror(v: int, r: int, size: int) -> int:
    r %= size
    m = (1 << size) - 1
    return ((v >> r) | (v << (size - r))) & m


def decode_bit_masks(n: int, imms: int, immr: int, immediate: bool, datasize: int):
    """DecodeBitMasks() from the Arm ARM shared pseudocode. Returns (wmask, tmask) or None if reserved."""
    combined = (n << 6) | (~imms & 0x3F)
    if combined == 0:
        return None
    length = combined.bit_length() - 1
    if length < 1:
        return None
    levels = (1 << length) - 1
    if immediate and (imms & levels) == levels:
        return None
    s = imms & levels
    r = immr & levels
    diff = (s - r) & levels
    esize = 1 << length
    if esize > datasize:
        return None
    welem = (1 << (s + 1)) - 1
    telem = (1 << (diff + 1)) - 1
    welem = _ror(welem, r, esize)

    def rep(e):
        out = 0
        for i in range(datasize // esize):
            out |= e << (i * esize)
        return out

    return rep(welem), rep(telem)


def _u(addr, word, reason):
    return Insn(addr, word, "unsupported", {"reason": reason})


# 0xE7FFDEFE: the trap word Nintendo's toolchain emits for unreachable code (it is the first instruction of
# rs_fatal_error in Tropical Freeze's main). It is unallocated in A64, so it raises an Undefined Instruction
# exception: the lifter turns it into a runtime trap instead of reporting it as unsupported.
UDF_TRAP_WORD = 0xE7FFDEFE


def decode(word: int, addr: int = 0) -> Insn:
    w = word
    if w == UDF_TRAP_WORD:
        return Insn(addr, w, "udf_trap", {})
    op0 = bits(w, 25, 4)
    if op0 in (0b1000, 0b1001):
        return _dp_imm(w, addr)
    if op0 in (0b1010, 0b1011):
        return _branch_sys(w, addr)
    if op0 & 0b0101 == 0b0100:
        return _ldst(w, addr)
    if op0 & 0b0111 == 0b0101:
        return _dp_reg(w, addr)
    if op0 & 0b0111 == 0b0111:
        return _simd(w, addr)
    if w == 0:
        return _u(addr, w, "udf/zero word")
    return _u(addr, w, "unallocated or reserved")


# --- data processing, immediate ------------------------------------------------------------------------------

def _dp_imm(w, a):
    sf = bits(w, 31, 1)
    rd, rn = bits(w, 0, 5), bits(w, 5, 5)
    op = bits(w, 23, 3)
    if op in (0b000, 0b001):  # ADR / ADRP
        imm = sext((bits(w, 5, 19) << 2) | bits(w, 29, 2), 21)
        if bits(w, 31, 1):
            return Insn(a, w, "movi", {"rd": rd, "sf": 1, "imm": ((a & ~0xFFF) + (imm << 12)) & (2**64 - 1)})
        return Insn(a, w, "movi", {"rd": rd, "sf": 1, "imm": (a + imm) & (2**64 - 1)})
    if op == 0b010:  # add/sub immediate
        sh = bits(w, 22, 1)
        imm = bits(w, 10, 12) << (12 if sh else 0)
        sub, setf = bits(w, 30, 1), bits(w, 29, 1)
        return Insn(a, w, "addsub_imm", {"sf": sf, "sub": sub, "setflags": setf, "imm": imm,
                                         "rn": SP if rn == 31 else rn,
                                         "rd": (ZR if setf else SP) if rd == 31 else rd})
    if op == 0b011:
        return _u(a, w, "add/sub immediate with tags")
    if op == 0b100:  # logical immediate
        n = bits(w, 22, 1)
        if not sf and n:
            return _u(a, w, "logical imm: N=1 with sf=0")
        size = 64 if sf else 32
        m = decode_bit_masks(n, bits(w, 10, 6), bits(w, 16, 6), True, size)
        if m is None:
            return _u(a, w, "logical imm: reserved mask")
        opc = bits(w, 29, 2)  # AND ORR EOR ANDS
        return Insn(a, w, "logic_imm", {"sf": sf, "opc": opc, "imm": m[0], "rn": rn,
                                        "rd": (ZR if opc == 3 else SP) if rd == 31 else rd})
    if op == 0b101:  # move wide
        opc = bits(w, 29, 2)
        hw = bits(w, 21, 2)
        if opc == 1 or (not sf and hw >= 2):
            return _u(a, w, "move wide: unallocated")
        return Insn(a, w, {0: "movn", 2: "movz", 3: "movk"}[opc],
                    {"sf": sf, "rd": rd, "imm16": bits(w, 5, 16), "shift": hw * 16})
    if op == 0b110:  # bitfield
        opc = bits(w, 29, 2)
        n = bits(w, 22, 1)
        if opc == 3 or n != sf:
            return _u(a, w, "bitfield: unallocated")
        immr, imms = bits(w, 16, 6), bits(w, 10, 6)
        if not sf and (immr >= 32 or imms >= 32):
            return _u(a, w, "bitfield: reserved")
        size = 64 if sf else 32
        wmask, tmask = decode_bit_masks(n, imms, immr, False, size)
        return Insn(a, w, "bitfield", {"sf": sf, "opc": opc, "rd": rd, "rn": rn, "immr": immr, "imms": imms,
                                       "wmask": wmask, "tmask": tmask})
    # op == 0b111: extract
    if bits(w, 29, 2) or bits(w, 21, 1) or bits(w, 22, 1) != sf:
        return _u(a, w, "extract: unallocated")
    lsb = bits(w, 10, 6)
    if not sf and lsb >= 32:
        return _u(a, w, "extract: reserved")
    return Insn(a, w, "extr", {"sf": sf, "rd": rd, "rn": rn, "rm": bits(w, 16, 5), "lsb": lsb})


# --- branches, exceptions, system ----------------------------------------------------------------------------

SYSREGS = {
    # (o0:op1:CRn:CRm:op2) as the 15-bit field of MRS/MSR, see the Arm ARM system register index.
    0x5E82: "tpidr_el0", 0x5E83: "tpidrro_el0", 0x5A10: "nzcv", 0x5A20: "fpcr", 0x5A21: "fpsr",
    0x5F02: "cntvct_el0", 0x5F00: "cntfrq_el0", 0x5807: "dczid_el0", 0x5F01: "cntpct_el0", 0x5801: "ctr_el0",
}
# SYS cache maintenance by address (op1=3, CRn=7, CRm, op2=1): DC CVAC, DC CVAU, DC CIVAC, IC IVAU. Guest memory
# is ordinary coherent host memory and code is never executed from it, so these have no effect to model.
CACHE_OPS = {10: "dc cvac", 11: "dc cvau", 14: "dc civac", 5: "ic ivau"}


def _branch_sys(w, a):
    if bits(w, 26, 5) == 0b00101:  # B / BL
        tgt = (a + (sext(bits(w, 0, 26), 26) << 2)) & (2**64 - 1)
        return Insn(a, w, "bl" if bits(w, 31, 1) else "b", {"target": tgt})
    if bits(w, 25, 6) == 0b011010:  # CBZ/CBNZ
        tgt = (a + (sext(bits(w, 5, 19), 19) << 2)) & (2**64 - 1)
        return Insn(a, w, "cbnz" if bits(w, 24, 1) else "cbz", {"sf": bits(w, 31, 1), "rt": bits(w, 0, 5),
                                                               "target": tgt})
    if bits(w, 25, 6) == 0b011011:  # TBZ/TBNZ
        tgt = (a + (sext(bits(w, 5, 14), 14) << 2)) & (2**64 - 1)
        return Insn(a, w, "tbnz" if bits(w, 24, 1) else "tbz",
                    {"rt": bits(w, 0, 5), "bit": (bits(w, 31, 1) << 5) | bits(w, 19, 5), "target": tgt})
    if bits(w, 24, 8) == 0b01010100 and not bits(w, 4, 1):
        tgt = (a + (sext(bits(w, 5, 19), 19) << 2)) & (2**64 - 1)
        return Insn(a, w, "bcond", {"cond": bits(w, 0, 4), "target": tgt})
    if w & 0xFFFFFC1F == 0xD61F0000:
        return Insn(a, w, "br", {"rn": bits(w, 5, 5)})
    if w & 0xFFFFFC1F == 0xD63F0000:
        return Insn(a, w, "blr", {"rn": bits(w, 5, 5)})
    if w & 0xFFFFFC1F == 0xD65F0000:
        return Insn(a, w, "ret", {"rn": bits(w, 5, 5)})
    if w & 0xFFE0001F == 0xD4000001:
        return Insn(a, w, "svc", {"imm": bits(w, 5, 16)})
    if w & 0xFFE0001F == 0xD4200000:
        return Insn(a, w, "brk", {"imm": bits(w, 5, 16)})
    if w & 0xFFFFF01F == 0xD503201F:
        # HINT space: NOP, YIELD, WFE, WFI, SEV, BTI and the PAC hints. Pointer authentication is not used by the
        # game (to verify) and hints have no architectural effect a single thread can observe.
        return Insn(a, w, "nop", {"hint": bits(w, 5, 7)})
    if w & 0xFFFFF01F == 0xD503301F:
        op2 = bits(w, 5, 3)
        if op2 in (4, 5, 6):  # DSB DMB ISB
            return Insn(a, w, "barrier", {"kind": {4: "dsb", 5: "dmb", 6: "isb"}[op2]})
        if op2 == 2:
            return Insn(a, w, "clrex", {})
        return _u(a, w, "system: barrier variant")
    if w & 0xFFF80000 == 0xD5080000:  # SYS
        op1, crn, crm, op2 = bits(w, 16, 3), bits(w, 12, 4), bits(w, 8, 4), bits(w, 5, 3)
        if op1 == 3 and crn == 7 and op2 == 1 and crm in CACHE_OPS:
            return Insn(a, w, "nop", {"hint": CACHE_OPS[crm]})
        return _u(a, w, "sys: not in subset (dc zva or other)")
    if w & 0xFFF00000 == 0xD5300000:  # MRS
        reg = bits(w, 5, 15)
        name = SYSREGS.get(reg)
        if name is None:
            return _u(a, w, f"mrs: system register {reg:#x}")
        return Insn(a, w, "mrs", {"rt": bits(w, 0, 5), "sysreg": name})
    if w & 0xFFF00000 == 0xD5100000:  # MSR (register)
        reg = bits(w, 5, 15)
        name = SYSREGS.get(reg)
        if name not in ("tpidr_el0", "nzcv", "fpcr", "fpsr"):
            return _u(a, w, f"msr: system register {reg:#x}")
        return Insn(a, w, "msr", {"rt": bits(w, 0, 5), "sysreg": name})
    return _u(a, w, "branch/system: not in subset")


# --- loads and stores ----------------------------------------------------------------------------------------

def _ldst_opc(size, opc):
    """(kind, nbytes, signed, dest 64-bit) for the integer load/store opc field; None if unallocated/prefetch."""
    if opc == 0:
        return ("st", 1 << size, False, size == 3)
    if opc == 1:
        return ("ld", 1 << size, False, size == 3)
    if size == 3:
        return ("prfm", 8, False, True) if opc == 2 else None
    if size == 2:
        return ("ld", 4, True, True) if opc == 2 else None
    return ("ld", 1 << size, True, opc == 2)


def _unpredictable_overlap(i: Insn) -> bool:
    """Register overlaps the Arm ARM makes CONSTRAINED UNPREDICTABLE for loads/stores (several outcomes are
    allowed, so there is no single behaviour to lift). Compilers do not emit them."""
    f = i.f
    if i.op in ("ldp", "vldp") and f["rt"] == f["rt2"]:
        return True
    if f.get("wb") and f["rn"] != SP and i.op in ("ld", "st", "ldp", "stp"):
        return f["rn"] in (f["rt"], f.get("rt2", -1))
    return False


def _ldst(w, a):
    i = _ldst_inner(w, a)
    if i.op != "unsupported" and _unpredictable_overlap(i):
        return _u(a, w, "constrained unpredictable register overlap")
    return i


def _ldst_inner(w, a):
    rt, rn = bits(w, 0, 5), bits(w, 5, 5)
    base = SP if rn == 31 else rn
    v = bits(w, 26, 1)
    if bits(w, 24, 6) == 0b001000 and not v:  # exclusive / acquire-release
        return _ldst_excl(w, a, rt, base)
    if w & 0xBF9F0000 == 0x0D000000 or w & 0xBF800000 == 0x0D800000:  # SIMD single structure
        return _ldst_simd_single(w, a, rt, base)
    if bits(w, 27, 3) == 0b101:  # load/store pair
        mode = bits(w, 23, 3)
        if v:  # SIMD&FP pair: S, D or Q registers
            opc, load = bits(w, 30, 2), bits(w, 22, 1)
            if opc == 3 or mode not in (0, 1, 2, 3):
                return _u(a, w, "fp/simd pair: unallocated")
            nbytes = 4 << opc
            return Insn(a, w, "vldp" if load else "vstp", {
                "rt": rt, "rt2": bits(w, 10, 5), "rn": base, "imm": sext(bits(w, 15, 7), 7) * nbytes,
                "nbytes": nbytes, "wb": {0: None, 1: "post", 2: None, 3: "pre"}[mode]})
        if mode not in (0, 1, 2, 3):
            return _u(a, w, "pair: unallocated")
        opc, load = bits(w, 30, 2), bits(w, 22, 1)
        if opc == 3 or (opc == 1 and not load) or (mode == 0 and opc == 1):
            return _u(a, w, "pair: unallocated opc")
        nbytes = 8 if opc == 2 else 4
        imm = sext(bits(w, 15, 7), 7) * nbytes
        wb = {0: None, 1: "post", 2: None, 3: "pre"}[mode]
        return Insn(a, w, "ldp" if load else "stp", {"rt": rt, "rt2": bits(w, 10, 5), "rn": base, "imm": imm,
                                                     "nbytes": nbytes, "signed": opc == 1, "wb": wb})
    if bits(w, 24, 6) & 0b111011 == 0b011000:  # load literal
        opc = bits(w, 30, 2)
        tgt = (a + (sext(bits(w, 5, 19), 19) << 2)) & (2**64 - 1)
        if v:
            if opc == 3:
                return _u(a, w, "fp/simd load literal: unallocated")
            return Insn(a, w, "vldr_lit", {"rt": rt, "lit": tgt, "nbytes": 4 << opc})
        if opc == 3:
            return Insn(a, w, "nop", {"hint": "prfm literal"})
        return Insn(a, w, "ldr_lit", {"rt": rt, "lit": tgt, "nbytes": 8 if opc == 1 else 4, "signed": opc == 2,
                                      "x": opc != 0})
    if bits(w, 27, 3) == 0b111:  # load/store register
        size, opc = bits(w, 30, 2), bits(w, 22, 2)
        if v:
            return _ldst_simd(w, a, rt, base, size, opc)
        k = _ldst_opc(size, opc)
        if k is None:
            return _u(a, w, "load/store: unallocated opc")
        kind, nbytes, signed, x = k
        f = {"rt": rt, "rn": base, "nbytes": nbytes, "signed": signed, "x": x}
        if bits(w, 24, 2) == 0b01:  # unsigned offset
            f.update(imm=bits(w, 10, 12) * nbytes, wb=None)
        elif bits(w, 24, 2) == 0b00 and bits(w, 21, 1) == 0:
            idx = bits(w, 10, 2)
            if idx == 2:
                return _u(a, w, "unprivileged load/store")
            f.update(imm=sext(bits(w, 12, 9), 9), wb={0: None, 1: "post", 3: "pre"}[idx])
            if kind == "prfm" and idx != 0:
                return _u(a, w, "prfm: unallocated index mode")
        elif bits(w, 24, 2) == 0b00 and bits(w, 10, 2) == 0b10:  # register offset
            option = bits(w, 13, 3)
            if option not in (2, 3, 6, 7):
                return _u(a, w, "load/store reg offset: bad option")
            shift = (size if bits(w, 12, 1) else 0)
            f.update(rm=bits(w, 16, 5), option=option, shift=shift, wb=None)
            return Insn(a, w, "nop" if kind == "prfm" else kind + "_reg", f)
        else:
            return _u(a, w, "load/store: atomic memory op or unallocated")
        if kind == "prfm":
            return Insn(a, w, "nop", {"hint": "prfm"})
        return Insn(a, w, kind, f)
    return _u(a, w, "load/store: not in subset")


def _ldst_simd(w, a, rt, base, size, opc):
    """LDR/STR of a B, H, S, D or Q register (unsigned offset, unscaled, pre/post-index, register offset)."""
    if opc >> 1:
        if size:
            return _u(a, w, "fp/simd load/store: unallocated size")
        nbytes = 16
    else:
        nbytes = 1 << size
    load = bool(opc & 1)
    f = {"rt": rt, "rn": base, "nbytes": nbytes}
    if bits(w, 24, 2) == 0b01:
        f.update(imm=bits(w, 10, 12) * nbytes, wb=None)
    elif bits(w, 24, 2) == 0b00 and bits(w, 21, 1) == 0:
        idx = bits(w, 10, 2)
        if idx == 2:
            return _u(a, w, "fp/simd load/store: unallocated index mode")
        f.update(imm=sext(bits(w, 12, 9), 9), wb={0: None, 1: "post", 3: "pre"}[idx])
    elif bits(w, 24, 2) == 0b00 and bits(w, 10, 2) == 0b10:
        option = bits(w, 13, 3)
        if option not in (2, 3, 6, 7):
            return _u(a, w, "fp/simd load/store reg offset: bad option")
        f.update(rm=bits(w, 16, 5), option=option, shift=(nbytes.bit_length() - 1) if bits(w, 12, 1) else 0,
                 wb=None)
        return Insn(a, w, "vld_reg" if load else "vst_reg", f)
    else:
        return _u(a, w, "fp/simd load/store: not in subset")
    return Insn(a, w, "vld" if load else "vst", f)


def vfp_expand_imm(imm8: int, n: int) -> int:
    """VFPExpandImm(): the 8-bit FMOV immediate as single (n=4) or double (n=8) bits."""
    sign, b6 = imm8 >> 7, (imm8 >> 6) & 1
    if n == 4:
        exp = ((b6 ^ 1) << 7) | ((0x1F if b6 else 0) << 2) | ((imm8 >> 4) & 3)
        return (sign << 31) | (exp << 23) | ((imm8 & 0xF) << 19)
    exp = ((b6 ^ 1) << 10) | ((0xFF if b6 else 0) << 2) | ((imm8 >> 4) & 3)
    return (sign << 63) | (exp << 52) | ((imm8 & 0xF) << 48)


FP1_OPS = {0: "fmov", 1: "fabs", 2: "fneg", 3: "fsqrt", 4: "fcvt", 5: "fcvt",
           8: "frintn", 9: "frintp", 10: "frintm", 11: "frintz", 12: "frinta", 14: "frintx", 15: "frinti"}
FP2_OPS = {0: "fmul", 1: "fdiv", 2: "fadd", 3: "fsub", 4: "fmax", 5: "fmin", 6: "fmaxnm", 7: "fminnm",
           8: "fnmul"}


def _fp_scalar(w, a, rd, rn, rm):
    """Scalar floating-point data processing, compare, select and immediate (single and double precision).
    Returns None when the word is not in these classes."""
    ftype = bits(w, 22, 2)
    klass = None
    if w & 0xFF207C00 == 0x1E204000:
        klass = "dp1"
    elif w & 0xFF200C00 == 0x1E200800:
        klass = "dp2"
    elif w & 0xFF000000 == 0x1F000000:
        klass = "dp3"
    elif w & 0xFF20FC07 == 0x1E202000:
        klass = "cmp"
    elif w & 0xFF200C00 == 0x1E200400:
        klass = "ccmp"
    elif w & 0xFF200C00 == 0x1E200C00:
        klass = "csel"
    elif w & 0xFF201FE0 == 0x1E201000:
        klass = "imm"
    if klass is None:
        return None
    if ftype > 1:
        return _u(a, w, f"fp {klass}: fp16 or unallocated type")
    n = 4 << ftype
    if klass == "dp1":
        opcode = bits(w, 15, 6)
        op = FP1_OPS.get(opcode)
        if op is None:
            return _u(a, w, "fp dp1: not in subset (fp16 conversion or unallocated)")
        if op == "fcvt":
            to = 4 if opcode == 4 else 8
            if to == n:
                return _u(a, w, "fp dp1: fcvt to the same type")
            return Insn(a, w, "fcvt", {"rd": rd, "rn": rn, "n": n, "to": to})
        return Insn(a, w, "fp1", {"kind": op, "rd": rd, "rn": rn, "n": n})
    if klass == "dp2":
        op = FP2_OPS.get(bits(w, 12, 4))
        if op is None:
            return _u(a, w, "fp dp2: unallocated")
        return Insn(a, w, "fp2", {"kind": op, "rd": rd, "rn": rn, "rm": rm, "n": n})
    if klass == "dp3":
        kind = {(0, 0): "fmadd", (0, 1): "fmsub", (1, 0): "fnmadd", (1, 1): "fnmsub"}[(bits(w, 21, 1), bits(w, 15, 1))]
        return Insn(a, w, "fp3", {"kind": kind, "rd": rd, "rn": rn, "rm": rm, "ra": bits(w, 10, 5), "n": n})
    if klass == "cmp":
        zero = bool(bits(w, 3, 1))
        if zero and rm:
            return _u(a, w, "fcmp #0.0 with Rm != 0")
        return Insn(a, w, "fcmp", {"rn": rn, "rm": rm, "zero": zero, "n": n})
    if klass == "ccmp":
        return Insn(a, w, "fccmp", {"rn": rn, "rm": rm, "cond": bits(w, 12, 4), "nzcv": bits(w, 0, 4), "n": n})
    if klass == "csel":
        return Insn(a, w, "fcsel", {"rd": rd, "rn": rn, "rm": rm, "cond": bits(w, 12, 4), "n": n})
    return Insn(a, w, "fmov_imm", {"rd": rd, "n": n, "imm": vfp_expand_imm(bits(w, 13, 8), n)})


def _simd(w, a):
    """The register-move part of Advanced SIMD and FP: FMOV general, INS, UMOV, DUP, MOVI, vector logic.
    FP arithmetic, conversions and vector arithmetic are not lifted yet."""
    rd, rn, rm = bits(w, 0, 5), bits(w, 5, 5), bits(w, 16, 5)
    fp = _fp_scalar(w, a, rd, rn, rm)
    if fp is not None:
        return fp
    if w & 0x7F20FC00 == 0x1E200000:  # conversion between FP and integer
        key = (bits(w, 31, 1), bits(w, 22, 2), bits(w, 19, 2), bits(w, 16, 3))
        forms = {(1, 1, 0, 6): ("fmov_to_gp", 0, 8), (1, 1, 0, 7): ("fmov_from_gp", 0, 8),
                 (0, 0, 0, 6): ("fmov_to_gp", 0, 4), (0, 0, 0, 7): ("fmov_from_gp", 0, 4),
                 (1, 2, 1, 6): ("fmov_to_gp", 1, 8), (1, 2, 1, 7): ("fmov_from_gp", 1, 8)}
        if key in forms:
            op, half, n = forms[key]
            return Insn(a, w, op, {"rd": rd, "rn": rn, "lane": half, "nbytes": n})
        ftype, rmode, opcode = bits(w, 22, 2), bits(w, 19, 2), bits(w, 16, 3)
        if ftype > 1:
            return _u(a, w, "fp/simd: fp<->int conversion (fp16 or unallocated)")
        n = 4 << ftype
        sf = bits(w, 31, 1)
        if opcode in (0, 1) or (rmode == 0 and opcode in (4, 5)):  # FCVT{N,P,M,Z,A}{S,U}
            mode = {0: "N", 1: "P", 2: "M", 3: "Z"}[rmode] if opcode in (0, 1) else "A"
            return Insn(a, w, "fcvt_int", {"rd": rd, "rn": rn, "n": n, "sf": sf, "mode": mode,
                                          "unsigned": bool(opcode & 1)})
        if rmode == 0 and opcode in (2, 3):  # SCVTF / UCVTF from a general register
            return Insn(a, w, "int_to_fp", {"rd": rd, "rn": rn, "n": n, "sf": sf, "unsigned": opcode == 3})
        return _u(a, w, "fp/simd: fp<->int conversion")
    if w & 0x7F200000 == 0x1E000000:  # conversion between FP and fixed point
        ftype, rmode, opcode, scale = bits(w, 22, 2), bits(w, 19, 2), bits(w, 16, 3), bits(w, 10, 6)
        sf = bits(w, 31, 1)
        if ftype > 1 or (not sf and scale < 32):
            return _u(a, w, "fp<->fixed: fp16, unallocated or reserved scale")
        n, fbits = 4 << ftype, 64 - scale
        if rmode == 3 and opcode in (0, 1):  # FCVTZS / FCVTZU
            return Insn(a, w, "fcvt_int", {"rd": rd, "rn": rn, "n": n, "sf": sf, "mode": "Z",
                                          "unsigned": bool(opcode), "fbits": fbits})
        if rmode == 0 and opcode in (2, 3):  # SCVTF / UCVTF
            return Insn(a, w, "int_to_fp", {"rd": rd, "rn": rn, "n": n, "sf": sf, "unsigned": opcode == 3,
                                            "fbits": fbits})
        return _u(a, w, "fp<->fixed: unallocated")
    if w & 0xDFBFFC00 == 0x5E21D800:  # SCVTF/UCVTF (vector, integer), scalar: Sd <- Sn, Dd <- Dn
        return Insn(a, w, "int_to_fp_v", {"rd": rd, "rn": rn, "n": 4 << bits(w, 22, 1),
                                          "unsigned": bool(bits(w, 29, 1))})
    if w & 0x9FE08400 == 0x0E000400:  # Advanced SIMD copy
        q, op, imm5, imm4 = bits(w, 30, 1), bits(w, 29, 1), bits(w, 16, 5), bits(w, 11, 4)
        esize_log = (imm5 & -imm5).bit_length() - 1 if imm5 & 0xF else None
        if esize_log is None:
            return _u(a, w, "simd copy: reserved imm5")
        index = imm5 >> (esize_log + 1)
        if op == 0 and imm4 == 3 and q:
            return Insn(a, w, "ins_gp", {"rd": rd, "rn": rn, "esize": 1 << esize_log, "index": index})
        if op == 0 and imm4 == 7 and (q == (esize_log == 3)):
            return Insn(a, w, "umov", {"rd": rd, "rn": rn, "esize": 1 << esize_log, "index": index})
        if op == 0 and imm4 == 1 and not (esize_log == 3 and not q):
            return Insn(a, w, "dup_gp", {"rd": rd, "rn": rn, "esize": 1 << esize_log, "q": q})
        if op == 0 and imm4 == 0 and not (esize_log == 3 and not q):  # DUP (element), vector
            return Insn(a, w, "vdup_elem", {"rd": rd, "rn": rn, "esize": 1 << esize_log, "index": index, "q": q})
        if op == 1 and q:  # INS (element): MOV Vd.T[i], Vn.T[j]
            return Insn(a, w, "vins_elem", {"rd": rd, "rn": rn, "esize": 1 << esize_log, "index": index,
                                            "src": imm4 >> esize_log})
        return _u(a, w, "simd copy: not in subset")
    if w & 0xFFE0FC00 == 0x5E000400:  # DUP (element), scalar: MOV Bd/Hd/Sd/Dd, Vn.T[i]
        imm5 = bits(w, 16, 5)
        if not imm5 & 0xF:
            return _u(a, w, "dup scalar: reserved imm5")
        esize_log = (imm5 & -imm5).bit_length() - 1
        return Insn(a, w, "dup_scalar", {"rd": rd, "rn": rn, "esize": 1 << esize_log,
                                         "index": imm5 >> (esize_log + 1)})
    if w & 0x9F20FC00 == 0x0E201C00:  # three same, logical: AND BIC ORR ORN EOR (BSL/BIT/BIF excluded)
        u, size, q = bits(w, 29, 1), bits(w, 22, 2), bits(w, 30, 1)
        if u and size:  # BSL, BIT, BIF
            return Insn(a, w, "vbitsel", {"rd": rd, "rn": rn, "rm": rm, "kind": size - 1, "q": q})
        kind = "eor" if u else {0: "and", 1: "bic", 2: "orr", 3: "orn"}[size]
        return Insn(a, w, "vlogic", {"rd": rd, "rn": rn, "rm": rm, "kind": kind, "q": q})
    if w & 0x9FF8FC00 == 0x0F00E400 and bits(w, 29, 1):  # MOVI 64-bit (op=1, cmode=1110)
        imm8 = (bits(w, 16, 3) << 5) | bits(w, 5, 5)
        imm = 0
        for k in range(8):
            if imm8 & (1 << k):
                imm |= 0xFF << (8 * k)
        return Insn(a, w, "movi64", {"rd": rd, "imm": imm, "q": bits(w, 30, 1)})
    v = _simd_vec(w, a, rd, rn, rm)
    return v if v is not None else _u(a, w, "fp/simd")


# --- Advanced SIMD vector ------------------------------------------------------------------------------------

VFP3 = {(0, 0, 0b11010): "add", (0, 1, 0b11010): "sub", (1, 0, 0b11011): "mul", (0, 0, 0b11011): "mulx",
        (1, 1, 0b11010): "abd", (1, 0, 0b11111): "div", (0, 0, 0b11110): "max", (0, 1, 0b11110): "min",
        (0, 0, 0b11000): "maxnm", (0, 1, 0b11000): "minnm", (0, 0, 0b11111): "recps", (0, 1, 0b11111): "rsqrts",
        (0, 0, 0b11100): "cmeq", (1, 0, 0b11100): "cmge", (1, 1, 0b11100): "cmgt", (1, 0, 0b11010): "addp",
        (1, 0, 0b11110): "maxp", (1, 1, 0b11110): "minp", (0, 0, 0b11001): "fmla", (0, 1, 0b11001): "fmls"}
VINT3 = {(0, 0b10000): "add", (1, 0b10000): "sub", (0, 0b10011): "mul", (1, 0b10001): "cmeq",
         (0, 0b00110): "cmgt", (0, 0b00111): "cmge", (1, 0b00110): "cmhi", (1, 0b00111): "cmhs",
         (0, 0b10001): "cmtst", (0, 0b10111): "addp", (0, 0b10010): "mla", (1, 0b10010): "mls"}
VFP2 = {(0, 1, 0b11101): "recpe", (1, 1, 0b11101): "rsqrte", (0, 1, 0b01111): "abs", (1, 1, 0b01111): "neg",
        (1, 1, 0b11111): "sqrt", (0, 1, 0b01100): "cmgt0", (0, 1, 0b01101): "cmeq0", (0, 1, 0b01110): "cmlt0",
        (1, 1, 0b01100): "cmge0", (1, 1, 0b01101): "cmle0", (0, 0, 0b11101): "scvtf", (1, 0, 0b11101): "ucvtf",
        (0, 1, 0b11011): "fcvtzs", (1, 1, 0b11011): "fcvtzu"}
VINT2 = {(0, 0b00000): "rev64", (1, 0b00000): "rev32", (0, 0b00001): "rev16", (0, 0b00101): "cnt",
         (1, 0b00101): "not", (0, 0b01000): "cmgt0", (1, 0b01000): "cmge0", (0, 0b01001): "cmeq0",
         (1, 0b01001): "cmle0", (0, 0b01010): "cmlt0", (0, 0b01011): "abs", (1, 0b01011): "neg"}


def adv_simd_expand_imm(op: int, cmode: int, imm8: int) -> int:
    """AdvSIMDExpandImm(): the 64-bit pattern of a modified-immediate instruction."""
    c = cmode >> 1
    if c == 0:
        return imm8 * 0x0000000100000001
    if c == 1:
        return (imm8 << 8) * 0x0000000100000001
    if c == 2:
        return (imm8 << 16) * 0x0000000100000001
    if c == 3:
        return (imm8 << 24) * 0x0000000100000001
    if c == 4:
        return imm8 * 0x0001000100010001
    if c == 5:
        return (imm8 << 8) * 0x0001000100010001
    if c == 6:
        v = ((imm8 << 8) | 0xFF) if not cmode & 1 else ((imm8 << 16) | 0xFFFF)
        return v * 0x0000000100000001
    if not cmode & 1:
        if not op:
            return imm8 * 0x0101010101010101
        return sum(0xFF << (8 * k) for k in range(8) if imm8 & (1 << k))
    if not op:  # FMOV single, replicated
        return vfp_expand_imm(imm8, 4) * 0x0000000100000001
    return vfp_expand_imm(imm8, 8)


def _esize_from_imm5(imm5):
    return (imm5 & -imm5).bit_length() - 1 if imm5 & 0xF else None


def _simd_vec(w, a, rd, rn, rm):
    """Vector Advanced SIMD classes; None when the word is in none of them."""
    q, u, size = bits(w, 30, 1), bits(w, 29, 1), bits(w, 22, 2)
    if w & 0x9F200400 == 0x0E200400:  # three same
        opc = bits(w, 11, 5)
        if opc >= 0b11000:
            sz = size & 1
            kind = VFP3.get((u, size >> 1, opc))
            if kind is None:
                return _u(a, w, "simd fp three-same: not in subset")
            if sz and not q:
                return _u(a, w, "simd fp three-same: reserved (2d needs Q)")
            return Insn(a, w, "vfp3", {"kind": kind, "rd": rd, "rn": rn, "rm": rm, "esize": 4 << sz, "q": q})
        kind = VINT3.get((u, opc))
        if kind is None or (size == 3 and not q) or (kind in ("mul", "mla", "mls") and size == 3):
            return _u(a, w, "simd int three-same: not in subset")
        return Insn(a, w, "vint3", {"kind": kind, "rd": rd, "rn": rn, "rm": rm, "esize": 1 << size, "q": q})
    if w & 0x9F200C00 == 0x0E200000:  # three different: widening multiply(-accumulate), add, subtract
        opc = bits(w, 12, 4)
        kind = {0b0000: "addl", 0b0010: "subl", 0b1000: "mlal", 0b1010: "mlsl", 0b1100: "mull"}.get(opc)
        if kind is None or size == 3:
            return _u(a, w, "simd three-different: not in subset")
        return Insn(a, w, "vwiden", {"kind": kind, "rd": rd, "rn": rn, "rm": rm, "esize": 1 << size,
                                     "signed": not u, "upper": q})
    if w & 0xFFFFFC00 == 0x5EF1B800:  # ADDP (scalar): Dd = Vn.D[0] + Vn.D[1]
        return Insn(a, w, "vaddp_scalar", {"rd": rd, "rn": rn})
    if w & 0x9F3E0C00 == 0x0E200800:  # two-register misc
        opc = bits(w, 12, 5)
        kind = VFP2.get((u, size >> 1, opc))
        if kind is not None and (opc >= 0b01100 and opc != 0b10010):
            sz = size & 1
            if sz and not q:
                return _u(a, w, "simd fp two-reg: reserved (2d needs Q)")
            return Insn(a, w, "vfp2", {"kind": kind, "rd": rd, "rn": rn, "esize": 4 << sz, "q": q})
        if opc == 0b10010 and not u:  # XTN / XTN2
            if size == 3:
                return _u(a, w, "xtn: reserved size")
            return Insn(a, w, "vxtn", {"rd": rd, "rn": rn, "esize": 1 << size, "upper": q})
        kind = VINT2.get((u, opc))
        if kind is None:
            return _u(a, w, "simd int two-reg: not in subset")
        if kind in ("cnt", "not") and size:
            return _u(a, w, "simd two-reg: rbit or reserved size")
        rev_limit = {"rev64": 2, "rev32": 1, "rev16": 0}.get(kind)
        if (rev_limit is not None and size > rev_limit) or (size == 3 and not q):
            return _u(a, w, "simd int two-reg: reserved size")
        return Insn(a, w, "vint2", {"kind": kind, "rd": rd, "rn": rn, "esize": 1 << size, "q": q})
    if w & 0x9F800400 == 0x0F000400 and bits(w, 19, 4):  # shift by immediate
        immh, opc = bits(w, 19, 4), bits(w, 11, 5)
        elog = immh.bit_length() - 1
        esize, immhb = 1 << elog, (immh << 3) | bits(w, 16, 3)
        if opc == 0b10100:  # SSHLL / USHLL (SXTL / UXTL)
            if elog == 3:
                return _u(a, w, "sshll: reserved")
            return Insn(a, w, "vshll", {"rd": rd, "rn": rn, "esize": esize, "shift": immhb - 8 * esize,
                                        "signed": not u, "upper": q})
        if elog == 3 and not q:
            return _u(a, w, "simd shift: reserved")
        if opc == 0b00000:  # SSHR / USHR
            return Insn(a, w, "vshift", {"rd": rd, "rn": rn, "esize": esize, "q": q, "kind": 2 if u else 1,
                                         "shift": 16 * esize - immhb})
        if opc == 0b01010 and not u:  # SHL
            return Insn(a, w, "vshift", {"rd": rd, "rn": rn, "esize": esize, "q": q, "kind": 0,
                                         "shift": immhb - 8 * esize})
        return _u(a, w, "simd shift: not in subset")
    scalar_idx = w & 0xDF000400 == 0x5F000000
    if w & 0x9F000400 == 0x0F000000 or scalar_idx:  # vector (or scalar) x indexed element
        opc, L, M, H, rm4 = bits(w, 12, 4), bits(w, 21, 1), bits(w, 20, 1), bits(w, 11, 1), bits(w, 16, 4)
        if size >> 1:  # floating point, size<0> = sz
            sz = size & 1
            if sz and L:
                return _u(a, w, "fp by element: reserved")
            index = H if sz else (H << 1) | L
            kind = {(0, 0b0001): "fmla", (0, 0b0101): "fmls", (0, 0b1001): "mul", (1, 0b1001): "mulx"}.get((u, opc))
            if kind is None or (sz and not q and not scalar_idx):
                return _u(a, w, "fp by element: not in subset")
            return Insn(a, w, "vfp_elem", {"kind": kind, "rd": rd, "rn": rn, "rm": (M << 4) | rm4,
                                           "esize": 4 << sz, "q": q, "index": index, "scalar": scalar_idx})
        if not scalar_idx and not u and opc == 0b1000 and size in (1, 2):  # MUL by element
            index, reg = ((H << 2) | (L << 1) | M, rm4) if size == 1 else ((H << 1) | L, (M << 4) | rm4)
            return Insn(a, w, "vmul_elem", {"rd": rd, "rn": rn, "rm": reg, "esize": 1 << size, "q": q,
                                            "index": index})
        return _u(a, w, "simd by element: not in subset")
    if w & 0xBF208C00 == 0x0E000800:  # permute: UZP, TRN, ZIP
        opc = bits(w, 12, 3)
        if opc in (0, 4) or (size == 3 and not q):
            return _u(a, w, "simd permute: unallocated")
        return Insn(a, w, "vperm", {"rd": rd, "rn": rn, "rm": rm, "op": opc, "esize": 1 << size, "q": q})
    if w & 0xBFE08400 == 0x2E000000:  # EXT
        imm4 = bits(w, 11, 4)
        if not q and imm4 >= 8:
            return _u(a, w, "ext: reserved")
        return Insn(a, w, "vext", {"rd": rd, "rn": rn, "rm": rm, "imm": imm4, "q": q})
    if w & 0xBFE08C00 == 0x0E000000:  # TBL / TBX
        return Insn(a, w, "vtbl", {"rd": rd, "rn": rn, "rm": rm, "nregs": bits(w, 13, 2) + 1,
                                   "tbx": bits(w, 12, 1), "q": q})
    if w & 0xBF3FFC00 == 0x0E31B800:  # ADDV
        if size == 3 or (size == 2 and not q):
            return _u(a, w, "addv: reserved")
        return Insn(a, w, "vaddv", {"rd": rd, "rn": rn, "esize": 1 << size, "q": q})
    if w & 0x9FF80400 == 0x0F000400:  # modified immediate: MOVI, MVNI, ORR, BIC, FMOV (vector)
        op, cmode, o2 = u, bits(w, 12, 4), bits(w, 11, 1)
        if o2:
            return _u(a, w, "fmov vector fp16")
        if cmode == 0b1111 and op and not q:
            return _u(a, w, "fmov 2d needs Q")
        imm8 = (bits(w, 16, 3) << 5) | bits(w, 5, 5)
        pattern = adv_simd_expand_imm(op, cmode, imm8)
        if (cmode & 0b1001) == 0b0001 or (cmode & 0b1101) == 0b1001:  # 0xx1 / 10x1: ORR or BIC
            return Insn(a, w, "vimm", {"rd": rd, "imm": pattern, "q": q, "kind": "bic" if op else "orr"})
        invert = op and cmode < 0b1110  # MVNI
        return Insn(a, w, "vimm", {"rd": rd, "imm": pattern ^ ((1 << 64) - 1) if invert else pattern, "q": q,
                                   "kind": "mov"})
    return None


def _ldst_simd_single(w, a, rt, base):
    """LD1/ST1 (single structure, one lane) and LD1R, optionally post-indexed."""
    q, load, r, opc, s, size = bits(w, 30, 1), bits(w, 22, 1), bits(w, 21, 1), bits(w, 13, 3), bits(w, 12, 1), \
        bits(w, 10, 2)
    post = bool(bits(w, 23, 1))
    rm = bits(w, 16, 5)
    if not post and rm:
        return _u(a, w, "simd single structure: unallocated")
    if r or opc & 1:
        return _u(a, w, "ld2/ld3/ld4 single structure")
    scale = opc >> 1
    f = {"rt": rt, "rn": base, "wb": ("imm" if rm == 31 else "reg") if post else None, "rm": rm}
    if scale == 3:
        if not load or s:
            return _u(a, w, "ld1r: unallocated")
        f.update(esize=1 << size, q=q)
        f["step"] = 1 << size
        return Insn(a, w, "vld1r", f)
    if scale == 0:
        esize, index = 1, (q << 3) | (s << 2) | size
    elif scale == 1:
        if size & 1:
            return _u(a, w, "ld1 h: unallocated")
        esize, index = 2, (q << 2) | (s << 1) | (size >> 1)
    elif size == 0:
        esize, index = 4, (q << 1) | s
    elif size == 1 and not s:
        esize, index = 8, q
    else:
        return _u(a, w, "ld1/st1 single: unallocated")
    f.update(esize=esize, index=index, step=esize)
    return Insn(a, w, "vld1lane" if load else "vst1lane", f)


def _ldst_excl(w, a, rt, base):
    size, o2, load, o1, o0 = bits(w, 30, 2), bits(w, 23, 1), bits(w, 22, 1), bits(w, 21, 1), bits(w, 15, 1)
    rs = bits(w, 16, 5)
    if o1:
        return _u(a, w, "exclusive pair / CAS")
    nbytes = 1 << size
    rt2 = bits(w, 10, 5)
    if o2:
        # LDAR/STLR need o0=1; o0=0 is LDLAR/STLLR (ARMv8.1 LORegions), absent on the Switch's ARMv8.0 cores.
        # Rs and Rt2 are should-be-one fields.
        if not o0:
            return _u(a, w, "ldlar/stllr (ARMv8.1)")
        if rs != 31 or rt2 != 31:
            return _u(a, w, "ldar/stlr: Rs/Rt2 not 11111")
        return Insn(a, w, "ldar" if load else "stlr", {"rt": rt, "rn": base, "nbytes": nbytes})
    if rt2 != 31 or (load and rs != 31):
        return _u(a, w, "ldxr/stxr: should-be-one fields not set")
    return Insn(a, w, "ldxr" if load else "stxr", {"rt": rt, "rn": base, "rs": rs, "nbytes": nbytes,
                                                    "ordered": bool(o0)})


# --- data processing, register -------------------------------------------------------------------------------

def _dp_reg(w, a):
    sf = bits(w, 31, 1)
    rd, rn, rm = bits(w, 0, 5), bits(w, 5, 5), bits(w, 16, 5)
    if bits(w, 24, 5) == 0b01010:  # logical shifted register
        imm6 = bits(w, 10, 6)
        if not sf and imm6 >= 32:
            return _u(a, w, "logical: reserved shift")
        return Insn(a, w, "logic_reg", {"sf": sf, "opc": bits(w, 29, 2), "invert": bits(w, 21, 1),
                                        "shift": bits(w, 22, 2), "amount": imm6, "rd": rd, "rn": rn, "rm": rm})
    if bits(w, 24, 5) == 0b01011:
        sub, setf = bits(w, 30, 1), bits(w, 29, 1)
        if not bits(w, 21, 1):  # shifted register
            shift, imm6 = bits(w, 22, 2), bits(w, 10, 6)
            if shift == 3 or (not sf and imm6 >= 32):
                return _u(a, w, "add/sub shifted: reserved")
            return Insn(a, w, "addsub_reg", {"sf": sf, "sub": sub, "setflags": setf, "shift": shift,
                                             "amount": imm6, "rd": rd, "rn": rn, "rm": rm})
        if bits(w, 22, 2) or bits(w, 10, 3) > 4:
            return _u(a, w, "add/sub extended: reserved")
        return Insn(a, w, "addsub_ext", {"sf": sf, "sub": sub, "setflags": setf, "option": bits(w, 13, 3),
                                         "amount": bits(w, 10, 3), "rm": rm, "rn": SP if rn == 31 else rn,
                                         "rd": (ZR if setf else SP) if rd == 31 else rd})
    if bits(w, 24, 5) == 0b11011:  # 3-source
        op31, o0, ra = bits(w, 21, 3), bits(w, 15, 1), bits(w, 10, 5)
        if bits(w, 29, 2):
            return _u(a, w, "dp 3-source: unallocated")
        if op31 == 0:
            return Insn(a, w, "msub" if o0 else "madd", {"sf": sf, "rd": rd, "rn": rn, "rm": rm, "ra": ra})
        if not sf:
            return _u(a, w, "dp 3-source: unallocated (sf=0)")
        if op31 in (1, 5):
            return Insn(a, w, "mull", {"signed": op31 == 1, "sub": o0, "rd": rd, "rn": rn, "rm": rm, "ra": ra})
        if op31 in (2, 6) and not o0:
            return Insn(a, w, "mulh", {"signed": op31 == 2, "rd": rd, "rn": rn, "rm": rm})
        return _u(a, w, "dp 3-source: not in subset")
    top = bits(w, 21, 8)
    if top == 0b11010000:  # ADC/SBC
        if bits(w, 10, 6):
            return _u(a, w, "adc/sbc: unallocated")
        return Insn(a, w, "adc", {"sf": sf, "sub": bits(w, 30, 1), "setflags": bits(w, 29, 1),
                                  "rd": rd, "rn": rn, "rm": rm})
    if top == 0b11010010:  # conditional compare
        if not bits(w, 29, 1) or bits(w, 10, 1) or bits(w, 4, 1):
            return _u(a, w, "ccmp: unallocated")
        return Insn(a, w, "ccmp", {"sf": sf, "sub": bits(w, 30, 1), "is_imm": bits(w, 11, 1), "rm": rm,
                                   "cond": bits(w, 12, 4), "rn": rn, "nzcv": bits(w, 0, 4)})
    if top == 0b11010100:  # conditional select
        if bits(w, 29, 1) or bits(w, 11, 1):
            return _u(a, w, "csel: unallocated")
        return Insn(a, w, "csel", {"sf": sf, "variant": (bits(w, 30, 1) << 1) | bits(w, 10, 1),
                                   "cond": bits(w, 12, 4), "rd": rd, "rn": rn, "rm": rm})
    if top == 0b11010110:
        opcode = bits(w, 10, 6)
        if bits(w, 29, 1):
            return _u(a, w, "dp 1/2-source: unallocated (S=1)")
        if not bits(w, 30, 1):  # 2-source
            ops = {2: "udiv", 3: "sdiv", 8: "lslv", 9: "lsrv", 10: "asrv", 11: "rorv"}
            if opcode not in ops:
                return _u(a, w, "dp 2-source: not in subset (crc32/pac/mte)")
            return Insn(a, w, ops[opcode], {"sf": sf, "rd": rd, "rn": rn, "rm": rm})
        if rm != 0:
            return _u(a, w, "dp 1-source: not in subset (pac)")
        if opcode == 0:
            return Insn(a, w, "rbit", {"sf": sf, "rd": rd, "rn": rn})
        if opcode == 1:
            return Insn(a, w, "rev", {"sf": sf, "rd": rd, "rn": rn, "container": 16})
        if opcode == 2:
            # sf=0: REV Wd (whole register); sf=1: REV32 Xd. Both reverse bytes inside each 32-bit container.
            return Insn(a, w, "rev", {"sf": sf, "rd": rd, "rn": rn, "container": 32})
        if opcode == 3 and sf:
            return Insn(a, w, "rev", {"sf": sf, "rd": rd, "rn": rn, "container": 64})
        if opcode == 4:
            return Insn(a, w, "clz", {"sf": sf, "rd": rd, "rn": rn})
        if opcode == 5:
            return Insn(a, w, "cls", {"sf": sf, "rd": rd, "rn": rn})
        return _u(a, w, "dp 1-source: unallocated")
    return _u(a, w, "dp register: not in subset")
