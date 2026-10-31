"""Lift A64 instructions to C that operates on `Cpu` (runtime/include/dkr/cpu.h).

One guest function becomes one C function `f_<addr>(Cpu *c)`. Branches inside the function become `goto`, BL
becomes a direct C call (the callee returns to the instruction after the BL, which is what BL/RET pairs do in
compiler-generated code), BLR/BR go through `rt_dispatch`, which maps c->pc to a lifted function.

Two exit modes:
  * "call"   - the normal mode above.
  * "pc"     - every exit stores the next guest pc in c->pc and returns. Used by the single-instruction
               differential tests, so taken and not-taken branches can be compared with a reference CPU.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Dict, Iterable, List, Optional, Set

from ..arm64.decoder import SP, ZR, Insn, decode

M64 = (1 << 64) - 1


def hx(v: int) -> str:
    return f"0x{v & M64:x}ull"


def fname(addr: int) -> str:
    return f"f_{addr:x}"


class LiftError(Exception):
    pass


# ---- operand helpers -----------------------------------------------------------------------------------------

def rd_(r: int, sf: int) -> str:
    """Read a register as a value of the operand width (64-bit: uint64_t, 32-bit: uint32_t)."""
    if r == ZR:
        return "0ull" if sf else "0u"
    if r == SP:
        return "c->sp" if sf else "(uint32_t)c->sp"
    return f"c->x[{r}]" if sf else f"(uint32_t)c->x[{r}]"


def wr_(r: int, sf: int, expr: str) -> str:
    """Write a register. 32-bit results zero-extend. Writes to ZR keep the expression for its side effects."""
    if r == ZR:
        return f"(void)({expr});"
    val = expr if sf else f"(uint64_t)(uint32_t)({expr})"
    if r == SP:
        return f"c->sp = {val};"
    return f"c->x[{r}] = {val};"


def shift_(val: str, typ: int, amount: int, sf: int) -> str:
    if amount == 0:
        return val
    if typ == 0:
        return f"({val} << {amount})" if sf else f"(uint32_t)({val} << {amount})"
    if typ == 1:
        return f"({val} >> {amount})"
    if typ == 2:
        return f"asr{64 if sf else 32}({val}, {amount})"
    return f"ror{64 if sf else 32}({val}, {amount})"


def extend_(r: int, option: int, amount: int, sf: int) -> str:
    v = rd_(r, 1)
    e = {0: f"(uint64_t)(uint8_t){v}", 1: f"(uint64_t)(uint16_t){v}", 2: f"(uint64_t)(uint32_t){v}", 3: v,
         4: f"(uint64_t)(int64_t)(int8_t){v}", 5: f"(uint64_t)(int64_t)(int16_t){v}",
         6: f"(uint64_t)(int64_t)(int32_t){v}", 7: v}[option]
    if amount:
        e = f"({e} << {amount})"
    return e if sf else f"(uint32_t)({e})"


def cmask(v: int, sf: int) -> str:
    return hx(v) if sf else f"0x{v & 0xFFFFFFFF:x}u"


# ---- per-instruction lifting ---------------------------------------------------------------------------------

@dataclass
class Ctx:
    """What the lifter knows about the surrounding function while lifting one instruction."""
    entry: int
    end: int
    exit_mode: str = "call"
    is_function: Callable[[int], bool] = lambda a: False
    unsupported: List[Insn] = field(default_factory=list)
    local_dispatch: bool = False  # BR may land inside this function: route it through a local switch

    def inside(self, t: int) -> bool:
        return self.entry <= t < self.end

    def leave(self, target_expr: str) -> List[str]:
        """Leave the function to a guest address (tail position)."""
        return [f"c->pc = {target_expr}; return;"] if self.exit_mode == "pc" else \
               [f"c->pc = {target_expr}; rt_dispatch(c); return;"]

    def jump(self, t: int) -> List[str]:
        # "pc" mode models exactly one step, so even a branch to an instruction of the same unit exits (a
        # `b .` would otherwise loop forever where the reference CPU executes it once).
        if self.exit_mode == "pc":
            return [f"c->pc = {hx(t)}; return;"]
        if self.inside(t):
            return [f"goto L_{t:x};"]
        if self.is_function(t):
            return [f"{fname(t)}(c); return;"]
        return self.leave(hx(t))

    def call(self, t: int, ret: int) -> List[str]:
        if self.exit_mode == "pc":
            return [f"c->x[30] = {hx(ret)}; c->pc = {hx(t)}; return;"]
        if self.is_function(t):
            return [f"c->x[30] = {hx(ret)}; {fname(t)}(c);"]
        return [f"c->x[30] = {hx(ret)}; c->pc = {hx(t)}; rt_dispatch(c);"]


def _load(nbytes: int, signed: bool, x: bool, addr: str) -> str:
    """Load expression as uint64_t: zero-extended, or sign-extended to 64 (x) or to 32 then zero-extended."""
    v = f"ld{nbytes * 8}({addr})"
    if not signed:
        return f"(uint64_t){v}"
    st = {1: "int8_t", 2: "int16_t", 4: "int32_t"}[nbytes]
    if x:
        return f"(uint64_t)(int64_t)({st}){v}"
    return f"(uint64_t)(uint32_t)(int32_t)({st}){v}"


def lift_insn(i: Insn, ctx: Ctx) -> List[str]:
    op, f = i.op, i.f
    nxt = i.addr + 4
    sf = f.get("sf", 1)
    W = 64 if sf else 32
    awc = f"awc{W}"
    T = "uint64_t" if sf else "uint32_t"

    if op == "unsupported":
        ctx.unsupported.append(i)
        return [f"rt_unsupported(c, {hx(i.addr)}, 0x{i.word:08x}u); /* {f['reason']} */"]
    if op == "nop":
        return []
    if op == "barrier":
        return ["rt_fence();"]
    if op == "clrex":
        return ["c->excl_valid = 0;"]
    if op == "movi":
        return [wr_(i.rd, 1, hx(i.imm))]
    if op == "addsub_imm":
        a = rd_(i.rn, sf)
        if i.setflags:
            b = cmask((~i.imm) if i.sub else i.imm, sf)
            return [wr_(i.rd, sf, f"{awc}(c, {a}, {b}, {1 if i.sub else 0}, 1)")]
        return [wr_(i.rd, sf, f"({T})({a} {'-' if i.sub else '+'} {cmask(i.imm, sf)})")]
    if op == "logic_imm":
        a = rd_(i.rn, sf)
        o = {0: "&", 1: "|", 2: "^", 3: "&"}[i.opc]
        expr = f"({T})({a} {o} {cmask(i.imm, sf)})"
        if i.opc == 3:
            return [f"{{ {T} r_ = {expr}; flags_logic{W}(c, r_); {wr_(i.rd, sf, 'r_')} }}"]
        return [wr_(i.rd, sf, expr)]
    if op == "movz":
        return [wr_(i.rd, sf, cmask(i.imm16 << i.shift, sf))]
    if op == "movn":
        return [wr_(i.rd, sf, cmask(~(i.imm16 << i.shift), sf))]
    if op == "movk":
        keep = cmask(~(0xFFFF << i.shift), sf)
        return [wr_(i.rd, sf, f"({rd_(i.rd, sf)} & {keep}) | {cmask(i.imm16 << i.shift, sf)}")]
    if op == "bitfield":
        src = rd_(i.rn, sf)
        wm, tm = cmask(i.wmask, sf), cmask(~i.wmask, sf)
        dst = rd_(i.rd, sf) if i.opc == 1 else ("0ull" if sf else "0u")
        bot = f"(({dst} & {tm}) | (ror{W}({src}, {i.immr}) & {wm}))"
        if i.opc == 0:  # SBFM: top is the replicated sign bit
            top = f"(({T})0 - (({src} >> {i.imms}) & 1))"
        else:
            top = dst
        tmask, ntmask = cmask(i.tmask, sf), cmask(~i.tmask, sf)
        return [wr_(i.rd, sf, f"({T})(({top} & {ntmask}) | ({bot} & {tmask}))")]
    if op == "extr":
        n, m = rd_(i.rn, sf), rd_(i.rm, sf)
        if i.lsb == 0:
            return [wr_(i.rd, sf, m)]
        return [wr_(i.rd, sf, f"({T})(({m} >> {i.lsb}) | ({n} << {W - i.lsb}))")]

    # --- branches
    if op == "b":
        return ctx.jump(i.target)
    if op == "bl":
        return ctx.call(i.target, nxt)
    if op in ("cbz", "cbnz"):
        test = f"{rd_(i.rt, sf)} {'==' if op == 'cbz' else '!='} 0"
        return [f"if ({test}) {{ {' '.join(ctx.jump(i.target))} }}"]
    if op in ("tbz", "tbnz"):
        test = f"(({rd_(i.rt, 1)} >> {i.bit}) & 1) {'==' if op == 'tbz' else '!='} 0"
        return [f"if ({test}) {{ {' '.join(ctx.jump(i.target))} }}"]
    if op == "bcond":
        if i.cond >= 14:
            return ctx.jump(i.target)
        return [f"if (cond_holds(c->nzcv, {i.cond})) {{ {' '.join(ctx.jump(i.target))} }}"]
    if op == "br":
        if ctx.local_dispatch:
            return [f"c->pc = {rd_(i.rn, 1)}; goto L_dispatch;"]
        return ctx.leave(rd_(i.rn, 1))
    if op == "blr":
        tgt = rd_(i.rn, 1)
        if ctx.exit_mode == "pc":
            return [f"{{ uint64_t t_ = {tgt}; c->x[30] = {hx(nxt)}; c->pc = t_; return; }}"]
        return [f"{{ uint64_t t_ = {tgt}; c->x[30] = {hx(nxt)}; c->pc = t_; rt_dispatch(c); }}"]
    if op == "ret":
        if ctx.exit_mode == "pc":
            return [f"c->pc = {rd_(i.rn, 1)}; return;"]
        return ["return;"]
    if op == "svc":
        return [f"rt_svc(c, {i.imm}, {hx(i.addr)});"]
    if op == "brk":
        return [f"rt_brk(c, {i.imm}, {hx(i.addr)});"]
    if op == "udf_trap":  # undefined-instruction trap used for unreachable code
        return [f"rt_unsupported(c, {hx(i.addr)}, 0x{i.word:08x}u); /* deliberate trap */"]
    if op == "mrs":
        src = {"tpidr_el0": "c->tpidr", "tpidrro_el0": "c->tpidrro", "nzcv": "c->nzcv", "fpcr": "c->fpcr",
               "fpsr": "c->fpsr", "cntvct_el0": "rt_cntvct()", "cntfrq_el0": "rt_cntfrq()",
               # DC ZVA is not lifted yet, so report it as prohibited (DZP=1, bit 4); block size 2^4 words.
               "dczid_el0": "0x14ull"}[i.sysreg]
        return [wr_(i.rt, 1, src)]
    if op == "msr":
        dst = {"tpidr_el0": "c->tpidr", "nzcv": "c->nzcv", "fpcr": "c->fpcr", "fpsr": "c->fpsr"}[i.sysreg]
        v = rd_(i.rt, 1)
        if i.sysreg == "nzcv":
            v = f"({v} & 0xF0000000ull)"
        return [f"{dst} = {v};"]

    # --- loads and stores
    if op in ("ld", "st", "ld_reg", "st_reg"):
        base = rd_(i.rn, 1)
        if op.endswith("_reg"):
            off = extend_(i.rm, i.option, i.shift, 1)
            addr, wb = f"{base} + {off}", None
        else:
            wb = i.wb
            addr = base if wb == "post" else (f"{base} + {hx(i.imm)}" if i.imm else base)
        lines = [f"uint64_t a_ = {addr};"]
        if op.startswith("ld"):
            lines.append(f"uint64_t v_ = {_load(i.nbytes, i.signed, i.x, 'a_')};")
        else:
            lines.append(f"st{i.nbytes * 8}(a_, {rd_(i.rt, 1)});")
        if wb == "pre":
            lines.append(wr_(i.rn, 1, "a_"))
        elif wb == "post":
            lines.append(wr_(i.rn, 1, f"a_ + {hx(i.imm)}"))
        if op.startswith("ld"):
            lines.append(wr_(i.rt, 1, "v_"))
        return ["{ " + " ".join(lines) + " }"]
    if op in ("ldp", "stp"):
        base = rd_(i.rn, 1)
        addr = base if i.wb == "post" else (f"{base} + {hx(i.imm)}" if i.imm else base)
        n = i.nbytes
        lines = [f"uint64_t a_ = {addr};"]
        x = n == 8 or i.signed
        if op == "ldp":
            lines.append(f"uint64_t v1_ = {_load(n, i.signed, x, 'a_')};")
            lines.append(f"uint64_t v2_ = {_load(n, i.signed, x, f'a_ + {n}')};")
        else:
            lines.append(f"st{n * 8}(a_, {rd_(i.rt, 1)}); st{n * 8}(a_ + {n}, {rd_(i.rt2, 1)});")
        if i.wb == "pre":
            lines.append(wr_(i.rn, 1, "a_"))
        elif i.wb == "post":
            lines.append(wr_(i.rn, 1, f"a_ + {hx(i.imm)}"))
        if op == "ldp":
            lines.append(wr_(i.rt, 1, "v1_"))
            lines.append(wr_(i.rt2, 1, "v2_"))
        return ["{ " + " ".join(lines) + " }"]
    if op == "ldr_lit":
        return [wr_(i.rt, 1, _load(i.nbytes, i.signed, i.x, hx(i.lit)))]
    if op == "ldar":
        return [wr_(i.rt, 1, _load(i.nbytes, False, i.nbytes == 8, rd_(i.rn, 1)))]
    if op == "stlr":
        # x86 keeps stores in order but may move a later load ahead of this store; A64 forbids that for an
        # STLR followed by an LDAR, so a full fence follows the store.
        return [f"st{i.nbytes * 8}({rd_(i.rn, 1)}, {rd_(i.rt, 1)}); rt_fence();"]
    if op == "ldxr":
        return [wr_(i.rt, 1, f"ldx(c, {rd_(i.rn, 1)}, {i.nbytes})")]
    if op == "stxr":
        return [f"{{ uint64_t s_ = stx(c, {rd_(i.rn, 1)}, {rd_(i.rt, 1)}, {i.nbytes}); {wr_(i.rs, 0, 's_')} }}"]

    # --- SIMD&FP loads, stores and register moves
    if op in ("vld", "vst", "vld_reg", "vst_reg"):
        base = rd_(i.rn, 1)
        if op.endswith("_reg"):
            addr, wb = f"{base} + {extend_(i.rm, i.option, i.shift, 1)}", None
        else:
            wb = i.wb
            addr = base if wb == "post" else (f"{base} + {hx(i.imm)}" if i.imm else base)
        lines = [f"uint64_t a_ = {addr};", f"{'vload' if op.startswith('vld') else 'vstore'}(c, {i.rt}, a_, {i.nbytes});"]
        if wb == "pre":
            lines.append(wr_(i.rn, 1, "a_"))
        elif wb == "post":
            lines.append(wr_(i.rn, 1, f"a_ + {hx(i.imm)}"))
        return ["{ " + " ".join(lines) + " }"]
    if op in ("vldp", "vstp"):
        base = rd_(i.rn, 1)
        addr = base if i.wb == "post" else (f"{base} + {hx(i.imm)}" if i.imm else base)
        fn_ = "vload" if op == "vldp" else "vstore"
        lines = [f"uint64_t a_ = {addr};", f"{fn_}(c, {i.rt}, a_, {i.nbytes});",
                 f"{fn_}(c, {i.rt2}, a_ + {i.nbytes}, {i.nbytes});"]
        if i.wb == "pre":
            lines.append(wr_(i.rn, 1, "a_"))
        elif i.wb == "post":
            lines.append(wr_(i.rn, 1, f"a_ + {hx(i.imm)}"))
        return ["{ " + " ".join(lines) + " }"]
    if op == "vldr_lit":
        return [f"vload(c, {i.rt}, {hx(i.lit)}, {i.nbytes});"]
    if op == "fmov_to_gp":
        src = f"c->v[{i.rn}].d[{i.lane}]" if i.nbytes == 8 else f"c->v[{i.rn}].s[0]"
        return [wr_(i.rd, 1, f"(uint64_t){src}")]
    if op == "fmov_from_gp":
        if i.lane:
            return [f"c->v[{i.rd}].d[1] = {rd_(i.rn, 1)};"]
        val = rd_(i.rn, 1) if i.nbytes == 8 else f"(uint64_t)(uint32_t){rd_(i.rn, 1)}"
        return [f"{{ uint64_t t_ = {val}; c->v[{i.rd}].d[0] = t_; c->v[{i.rd}].d[1] = 0; }}"]
    if op == "ins_gp":
        return [f"vset_elem(c, {i.rd}, {i.esize}, {i.index}, {rd_(i.rn, 1)});"]
    if op == "umov":
        return [wr_(i.rd, 1, f"velem(c, {i.rn}, {i.esize}, {i.index})")]
    if op == "dup_gp":
        hi = "p_" if i.q else "0"
        return [f"{{ uint64_t p_ = vrep({rd_(i.rn, 1)}, {i.esize}); c->v[{i.rd}].d[0] = p_; c->v[{i.rd}].d[1] = {hi}; }}"]
    if op == "dup_scalar":
        return [f"{{ uint64_t e_ = velem(c, {i.rn}, {i.esize}, {i.index}); c->v[{i.rd}].d[0] = e_; "
                f"c->v[{i.rd}].d[1] = 0; }}"]
    if op == "vlogic":
        expr = {"and": "n_ & m_", "bic": "n_ & ~m_", "orr": "n_ | m_", "orn": "n_ | ~m_", "eor": "n_ ^ m_"}[i.kind]
        parts = []
        for k in range(2 if i.q else 1):
            parts.append(f"{{ uint64_t n_ = c->v[{i.rn}].d[{k}], m_ = c->v[{i.rm}].d[{k}]; r_[{k}] = {expr}; }}")
        return [f"{{ uint64_t r_[2] = {{0, 0}}; {' '.join(parts)} c->v[{i.rd}].d[0] = r_[0]; c->v[{i.rd}].d[1] = r_[1]; }}"]
    if op == "movi64":
        hi = hx(i.imm) if i.q else "0"
        return [f"c->v[{i.rd}].d[0] = {hx(i.imm)}; c->v[{i.rd}].d[1] = {hi};"]

    # --- scalar floating point (runtime/include/dkr/fp.h has the A64 semantics)
    if op in FP_OPS:
        return _lift_fp(i, ctx)
    # --- vector Advanced SIMD (runtime/include/dkr/simd.h)
    if op in VEC_OPS:
        return _lift_vec(i, ctx)

    # --- data processing, register
    if op == "addsub_reg":
        a, b = rd_(i.rn, sf), shift_(rd_(i.rm, sf), i.shift, i.amount, sf)
        if i.setflags:
            nb = f"({T})~({b})" if i.sub else b
            return [wr_(i.rd, sf, f"{awc}(c, {a}, {nb}, {1 if i.sub else 0}, 1)")]
        return [wr_(i.rd, sf, f"({T})({a} {'-' if i.sub else '+'} {b})")]
    if op == "addsub_ext":
        a, b = rd_(i.rn, sf), extend_(i.rm, i.option, i.amount, sf)
        if i.setflags:
            nb = f"({T})~({b})" if i.sub else b
            return [wr_(i.rd, sf, f"{awc}(c, {a}, {nb}, {1 if i.sub else 0}, 1)")]
        return [wr_(i.rd, sf, f"({T})({a} {'-' if i.sub else '+'} {b})")]
    if op == "logic_reg":
        a, b = rd_(i.rn, sf), shift_(rd_(i.rm, sf), i.shift, i.amount, sf)
        if i.invert:
            b = f"({T})~{b}"
        o = {0: "&", 1: "|", 2: "^", 3: "&"}[i.opc]
        expr = f"({T})({a} {o} {b})"
        if i.opc == 3:
            return [f"{{ {T} r_ = {expr}; flags_logic{W}(c, r_); {wr_(i.rd, sf, 'r_')} }}"]
        return [wr_(i.rd, sf, expr)]
    if op in ("madd", "msub"):
        prod = f"({T})({rd_(i.rn, sf)} * {rd_(i.rm, sf)})"
        return [wr_(i.rd, sf, f"({T})({rd_(i.ra, sf)} {'-' if op == 'msub' else '+'} {prod})")]
    if op == "mull":
        cast = "(uint64_t)(int64_t)(int32_t)" if i.signed else "(uint64_t)(uint32_t)"
        prod = f"({cast}{rd_(i.rn, 1)} * {cast}{rd_(i.rm, 1)})"
        return [wr_(i.rd, 1, f"{rd_(i.ra, 1)} {'-' if i.sub else '+'} {prod}")]
    if op == "mulh":
        return [wr_(i.rd, 1, f"{'smulh64' if i.signed else 'umulh64'}({rd_(i.rn, 1)}, {rd_(i.rm, 1)})")]
    if op == "adc":
        b = rd_(i.rm, sf)
        if i.sub:
            b = f"({T})~{b}"
        return [wr_(i.rd, sf, f"{awc}(c, {rd_(i.rn, sf)}, {b}, (c->nzcv >> 29) & 1, {i.setflags})")]
    if op == "ccmp":
        a = rd_(i.rn, sf)
        b = cmask(i.rm, sf) if i.is_imm else rd_(i.rm, sf)
        if i.sub:
            b = f"({T})~({b})"
        return [f"if (cond_holds(c->nzcv, {i.cond})) (void){awc}(c, {a}, {b}, {1 if i.sub else 0}, 1); "
                f"else c->nzcv = {hx(i.nzcv << 28)};"]
    if op == "csel":
        n, m = rd_(i.rn, sf), rd_(i.rm, sf)
        alt = {0: m, 1: f"({T})({m} + 1)", 2: f"({T})~{m}", 3: f"({T})(0 - {m})"}[i.variant]
        return [wr_(i.rd, sf, f"cond_holds(c->nzcv, {i.cond}) ? {n} : {alt}")]
    if op in ("udiv", "sdiv"):
        return [wr_(i.rd, sf, f"{op}{W}({rd_(i.rn, sf)}, {rd_(i.rm, sf)})")]
    if op in ("lslv", "lsrv", "asrv", "rorv"):
        a, s = rd_(i.rn, sf), f"({rd_(i.rm, sf)} & {W - 1})"
        expr = {"lslv": f"({T})({a} << {s})", "lsrv": f"({a} >> {s})", "asrv": f"asr{W}({a}, (unsigned){s})",
                "rorv": f"ror{W}({a}, (unsigned){s})"}[op]
        return [wr_(i.rd, sf, expr)]
    if op == "rbit":
        return [wr_(i.rd, sf, f"rbit{W}({rd_(i.rn, sf)})")]
    if op == "rev":
        return [wr_(i.rd, sf, f"rev_lanes({rd_(i.rn, sf)}, {i.container})")]
    if op in ("clz", "cls"):
        return [wr_(i.rd, sf, f"{op}{W}({rd_(i.rn, sf)})")]
    raise LiftError(f"no lifting rule for {i!r}")


# ---- scalar floating point -----------------------------------------------------------------------------------

FP_OPS = {"fp1", "fp2", "fp3", "fcvt", "fcmp", "fccmp", "fcsel", "fmov_imm", "fcvt_int", "int_to_fp", "int_to_fp_v"}
ROUND = {"N": "DKR_RN", "P": "DKR_RP", "M": "DKR_RM", "Z": "DKR_RZ", "A": "DKR_RA"}


def _fv(r: int, n: int) -> str:
    """Low single (n=4) or double (n=8) of a V register, as bits."""
    return f"c->v[{r}].s[0]" if n == 4 else f"c->v[{r}].d[0]"


def _fset(rd: int, n: int, expr: str) -> str:
    return f"vset_{'s' if n == 4 else 'd'}(c, {rd}, {expr});"


def _lift_fp(i: Insn, ctx: Ctx) -> List[str]:
    op, n = i.op, i.f.get("n", 4)
    w = 32 if n == 4 else 64
    sign = "F32_SIGN" if n == 4 else "F64_SIGN"
    if op == "fp1":
        a = _fv(i.rn, n)
        expr = {"fmov": a, "fabs": f"({a} & ~{sign})", "fneg": f"({a} ^ {sign})", "fsqrt": f"fsqrt{w}({a})",
                "frintn": f"frint{w}({a}, DKR_RN)", "frintp": f"frint{w}({a}, DKR_RP)",
                "frintm": f"frint{w}({a}, DKR_RM)", "frintz": f"frint{w}({a}, DKR_RZ)",
                "frinta": f"frint{w}({a}, DKR_RA)",
                # X and I round with FPCR.RMode, which is round-to-nearest under the modelled FPCR = 0.
                "frintx": f"frint{w}({a}, DKR_RN)", "frinti": f"frint{w}({a}, DKR_RN)"}[i.kind]
        return [_fset(i.rd, n, expr)]
    if op == "fcvt":
        if n == 4:
            return [_fset(i.rd, 8, f"fcvt_s2d({_fv(i.rn, 4)})")]
        return [_fset(i.rd, 4, f"fcvt_d2s({_fv(i.rn, 8)})")]
    if op == "fp2":
        a, b = _fv(i.rn, n), _fv(i.rm, n)
        if i.kind == "fnmul":
            return [_fset(i.rd, n, f"(fmul{w}({a}, {b}) ^ {sign})")]
        return [_fset(i.rd, n, f"{i.kind}{w}({a}, {b})")]
    if op == "fp3":
        ra, rn, rm = _fv(i.ra, n), _fv(i.rn, n), _fv(i.rm, n)
        neg = lambda v: f"({v} ^ {sign})"  # FPNeg flips the sign of NaNs too, before NaN processing
        addend, mul1 = {"fmadd": (ra, rn), "fmsub": (ra, neg(rn)), "fnmadd": (neg(ra), neg(rn)),
                        "fnmsub": (neg(ra), rn)}[i.kind]
        return [_fset(i.rd, n, f"fmadd{w}({addend}, {mul1}, {rm})")]
    if op == "fcmp":
        b = "0" if i.zero else _fv(i.rm, n)
        return [f"c->nzcv = fcmp{w}({_fv(i.rn, n)}, {b});"]
    if op == "fccmp":
        return [f"c->nzcv = cond_holds(c->nzcv, {i.cond}) ? fcmp{w}({_fv(i.rn, n)}, {_fv(i.rm, n)}) "
                f": {hx(i.nzcv << 28)};"]
    if op == "fcsel":
        return [_fset(i.rd, n, f"cond_holds(c->nzcv, {i.cond}) ? {_fv(i.rn, n)} : {_fv(i.rm, n)}")]
    if op == "fmov_imm":
        return [_fset(i.rd, n, hx(i.imm) if n == 8 else f"0x{i.imm:x}u")]
    if op == "fcvt_int":
        x = f"(double)f32_of({_fv(i.rn, 4)})" if n == 4 else f"f64_of({_fv(i.rn, 8)})"
        fbits = i.f.get("fbits", 0)
        if fbits:  # x * 2^fbits is exact in double (no underflow when scaling up; overflow saturates)
            x = f"ldexp({x}, {fbits})"
        return [wr_(i.rd, i.sf, f"fcvt_to_int({x}, {ROUND[i.mode]}, {int(i.unsigned)}, {64 if i.sf else 32})")]
    if op == "int_to_fp":
        src = rd_(i.rn, i.sf)
        val = (f"(uint64_t){src}" if i.unsigned else
               (f"(int64_t){src}" if i.sf else f"(int64_t)(int32_t){src}"))
        fbits = i.f.get("fbits", 0)
        if fbits:  # one rounding at the conversion; the scaling by 2^-fbits is exact (result stays normal)
            fn = f"{'u' if i.unsigned else 's'}cvtfx{w}"
            return [_fset(i.rd, n, f"{fn}({val}, {fbits})")]
        return [_fset(i.rd, n, f"{'u' if i.unsigned else 's'}cvtf{w}({val})")]
    if op == "int_to_fp_v":
        src = _fv(i.rn, n)
        if n == 4:
            val = f"(uint64_t){src}" if i.unsigned else f"(int64_t)(int32_t){src}"
        else:
            val = src if i.unsigned else f"(int64_t){src}"
        return [_fset(i.rd, n, f"{'u' if i.unsigned else 's'}cvtf{w}({val})")]
    raise LiftError(f"no fp lifting rule for {i!r}")


# ---- vector Advanced SIMD ------------------------------------------------------------------------------------

VEC_OPS = {"vfp3", "vint3", "vfp2", "vint2", "vxtn", "vshll", "vshift", "vfp_elem", "vmul_elem", "vperm", "vext",
           "vtbl", "vaddv", "vimm", "vdup_elem", "vins_elem", "vbitsel", "vld1lane", "vst1lane", "vld1r",
           "vwiden", "vaddp_scalar"}
VF_KIND = {"add": "VF_ADD", "sub": "VF_SUB", "mul": "VF_MUL", "div": "VF_DIV", "max": "VF_MAX", "min": "VF_MIN",
           "maxnm": "VF_MAXNM", "minnm": "VF_MINNM", "mulx": "VF_MULX", "recps": "VF_RECPS", "rsqrts": "VF_RSQRTS",
           "cmeq": "VF_CMEQ", "cmge": "VF_CMGE", "cmgt": "VF_CMGT", "abd": "VF_ABD", "addp": "VF_ADD",
           "maxp": "VF_MAX", "minp": "VF_MIN"}
VU_KIND = {"recpe": "VU_RECPE", "rsqrte": "VU_RSQRTE", "abs": "VU_ABS", "neg": "VU_NEG", "sqrt": "VU_SQRT",
           "cmgt0": "VU_CMGT0", "cmeq0": "VU_CMEQ0", "cmlt0": "VU_CMLT0", "cmge0": "VU_CMGE0", "cmle0": "VU_CMLE0",
           "scvtf": "VU_SCVTF", "ucvtf": "VU_UCVTF", "fcvtzs": "VU_FCVTZS", "fcvtzu": "VU_FCVTZU"}
VI_KIND = {"add": "VI_ADD", "sub": "VI_SUB", "mul": "VI_MUL", "cmeq": "VI_CMEQ", "cmgt": "VI_CMGT",
           "cmge": "VI_CMGE", "cmhi": "VI_CMHI", "cmhs": "VI_CMHS", "cmtst": "VI_CMTST", "addp": "VI_ADDP"}
VI2_KIND = {"rev16": "VI2_REV16", "rev32": "VI2_REV32", "rev64": "VI2_REV64", "cnt": "VI2_CNT", "not": "VI2_NOT",
            "cmgt0": "VI2_CMGT0", "cmeq0": "VI2_CMEQ0", "cmlt0": "VI2_CMLT0", "cmge0": "VI2_CMGE0",
            "cmle0": "VI2_CMLE0", "abs": "VI2_ABS", "neg": "VI2_NEG"}


def _lanes(i: Insn) -> int:
    return (16 if i.q else 8) // i.esize


def _setv(rd: int, expr: str) -> List[str]:
    return [f"{{ V128 r_ = {expr}; c->v[{rd}] = r_; }}"]


def _lift_vec(i: Insn, ctx: Ctx) -> List[str]:
    op = i.op
    V = lambda r: f"c->v[{r}]"
    if op == "vfp3":
        if i.kind in ("fmla", "fmls"):
            return _setv(i.rd, f"vfmla({V(i.rd)}, {V(i.rn)}, {V(i.rm)}, {i.esize}, {_lanes(i)}, "
                               f"{int(i.kind == 'fmls')}, -1)")
        pair = int(i.kind in ("addp", "maxp", "minp"))
        return _setv(i.rd, f"vfp3({V(i.rn)}, {V(i.rm)}, {VF_KIND[i.kind]}, {i.esize}, {_lanes(i)}, {pair})")
    if op == "vfp2":
        return _setv(i.rd, f"vfp2({V(i.rn)}, {VU_KIND[i.kind]}, {i.esize}, {_lanes(i)})")
    if op == "vint3" and i.kind in ("mla", "mls"):
        return _setv(i.rd, f"vmla({V(i.rd)}, {V(i.rn)}, {V(i.rm)}, {i.esize}, {_lanes(i)}, {int(i.kind == 'mls')})")
    if op == "vwiden":
        kind = {"addl": "VW_ADDL", "subl": "VW_SUBL", "mlal": "VW_MLAL", "mlsl": "VW_MLSL", "mull": "VW_MULL"}[i.kind]
        return _setv(i.rd, f"vwiden({V(i.rd)}, {V(i.rn)}, {V(i.rm)}, {kind}, {int(i.signed)}, {i.esize}, {i.upper})")
    if op == "vaddp_scalar":
        return [f"{{ uint64_t s_ = c->v[{i.rn}].d[0] + c->v[{i.rn}].d[1]; vset_d(c, {i.rd}, s_); }}"]
    if op == "vint3":
        return _setv(i.rd, f"vint3({V(i.rn)}, {V(i.rm)}, {VI_KIND[i.kind]}, {i.esize}, {_lanes(i)})")
    if op == "vint2":
        return _setv(i.rd, f"vint2({V(i.rn)}, {VI2_KIND[i.kind]}, {i.esize}, {_lanes(i)})")
    if op == "vxtn":
        return _setv(i.rd, f"vxtn({V(i.rd)}, {V(i.rn)}, {i.esize}, {i.upper})")
    if op == "vshll":
        return _setv(i.rd, f"vshll({V(i.rn)}, {int(i.signed)}, {i.shift}, {i.esize}, {i.upper})")
    if op == "vshift":
        return _setv(i.rd, f"vshift({V(i.rn)}, {i.kind}, {i.shift}, {i.esize}, {_lanes(i)})")
    if op == "vfp_elem":
        lanes = 1 if i.scalar else _lanes(i)
        if i.kind in ("fmla", "fmls"):
            return _setv(i.rd, f"vfmla({V(i.rd)}, {V(i.rn)}, {V(i.rm)}, {i.esize}, {lanes}, "
                               f"{int(i.kind == 'fmls')}, {i.index})")
        return _setv(i.rd, f"vfp_elem({V(i.rn)}, {V(i.rm)}, {VF_KIND[i.kind]}, {i.esize}, {lanes}, {i.index})")
    if op == "vmul_elem":
        return _setv(i.rd, f"vint3({V(i.rn)}, vdup_elem({V(i.rm)}, {i.esize}, {i.index}, {_lanes(i)}), VI_MUL, "
                           f"{i.esize}, {_lanes(i)})")
    if op == "vperm":
        # The permute opcode is stored as f["op"]; `i.op` is the instruction kind.
        return _setv(i.rd, f"vperm({V(i.rn)}, {V(i.rm)}, {i.f['op']}, {i.esize}, {_lanes(i)})")
    if op == "vext":
        return _setv(i.rd, f"vext({V(i.rn)}, {V(i.rm)}, {i.imm}, {i.q})")
    if op == "vtbl":
        return _setv(i.rd, f"vtbl(c, {V(i.rd)}, {V(i.rm)}, {i.rn}, {i.nregs}, {i.tbx}, {i.q})")
    if op == "vaddv":
        return _setv(i.rd, f"vrep64(vaddv({V(i.rn)}, {i.esize}, {(16 if i.q else 8) // i.esize}), 0)")
    if op == "vimm":
        pat = hx(i.imm)
        if i.kind == "mov":
            return _setv(i.rd, f"vrep64({pat}, {i.q})")
        o = "|" if i.kind == "orr" else "& ~"
        hi = f"c->v[{i.rd}].d[1] {o} {pat}" if i.q else "0"
        return [f"{{ uint64_t lo_ = c->v[{i.rd}].d[0] {o} {pat}, hi_ = {hi}; c->v[{i.rd}].d[0] = lo_; "
                f"c->v[{i.rd}].d[1] = hi_; }}"]
    if op == "vdup_elem":
        return _setv(i.rd, f"vdup_elem({V(i.rn)}, {i.esize}, {i.index}, {_lanes(i)})")
    if op == "vins_elem":
        return [f"{{ uint64_t e_ = velem(c, {i.rn}, {i.esize}, {i.src}); vset_elem(c, {i.rd}, {i.esize}, "
                f"{i.index}, e_); }}"]
    if op == "vbitsel":
        return _setv(i.rd, f"vbitsel({V(i.rd)}, {V(i.rn)}, {V(i.rm)}, {i.kind}, {i.q})")
    if op in ("vld1lane", "vst1lane", "vld1r"):
        base = rd_(i.rn, 1)
        lines = [f"uint64_t a_ = {base};"]
        n = i.esize
        ld = {1: "ld8", 2: "ld16", 4: "ld32", 8: "ld64"}[n]
        st = {1: "st8", 2: "st16", 4: "st32", 8: "st64"}[n]
        if op == "vld1lane":
            lines.append(f"vset_elem(c, {i.rt}, {n}, {i.index}, {ld}(a_));")
        elif op == "vst1lane":
            lines.append(f"{st}(a_, velem(c, {i.rt}, {n}, {i.index}));")
        else:
            lines.append(f"{{ V128 r_ = vrep64(vrep({ld}(a_), {n}), {i.q}); c->v[{i.rt}] = r_; }}")
        if i.wb == "imm":
            lines.append(wr_(i.rn, 1, f"a_ + {i.step}"))
        elif i.wb == "reg":
            lines.append(wr_(i.rn, 1, f"a_ + {rd_(i.rm, 1)}"))
        return ["{ " + " ".join(lines) + " }"]
    raise LiftError(f"no vector lifting rule for {i!r}")


# ---- functions -----------------------------------------------------------------------------------------------

@dataclass
class Function:
    entry: int
    end: int
    name: str = ""


def branch_targets(insns: Iterable[Insn]) -> Set[int]:
    out = set()
    for i in insns:
        if i.op in ("b", "cbz", "cbnz", "tbz", "tbnz", "bcond"):
            out.add(i.target)
    return out


TERMINATORS = {"b", "br", "ret", "brk", "udf_trap"}


def lift_function(code: bytes, base: int, fn: Function, ctx_args: Optional[dict] = None,
                  exit_mode: str = "call") -> (List[str], List[Insn]):
    """Lift guest function [fn.entry, fn.end) whose bytes start at `base` in `code`."""
    ctx = Ctx(fn.entry, fn.end, exit_mode=exit_mode, **(ctx_args or {}))
    insns = []
    for a in range(fn.entry, fn.end, 4):
        off = a - base
        insns.append(decode(int.from_bytes(code[off:off + 4], "little"), a))
    labels = {t for t in branch_targets(insns) if ctx.inside(t)}
    # A computed BR (jump tables, mostly) can target any instruction of the function. Until jump tables are
    # resolved statically, every instruction gets a label and BR goes through a switch on c->pc; targets outside
    # the function fall back to rt_dispatch (tail calls through a register).
    ctx.local_dispatch = exit_mode == "call" and any(i.op == "br" for i in insns)
    if ctx.local_dispatch:
        labels = {i.addr for i in insns}
    out = [f"void {fname(fn.entry)}(Cpu *c) {{" + (f" /* {fn.name} */" if fn.name else "")]
    for i in insns:
        if i.addr in labels:
            out.append(f"L_{i.addr:x}:;")
        body = lift_insn(i, ctx)
        out.append(f"    /* {i.addr:x}: {i.word:08x} {i.op} */ " + " ".join(body))
    last = insns[-1] if insns else None
    if last is None or last.op not in TERMINATORS:
        out.append("    " + " ".join(ctx.leave(hx(fn.end))))
    if ctx.local_dispatch:
        out.append("L_dispatch:")
        out.append("    switch (c->pc) {")
        out += [f"    case {hx(i.addr)}: goto L_{i.addr:x};" for i in insns]
        out.append("    default: rt_dispatch(c); return;")
        out.append("    }")
    out.append("}")
    return out, ctx.unsupported


C_PRELUDE = """/* GENERATED BY DKRecomp - do not edit. Derived from the input binary: keep out of version control. */
#include "dkr/cpu.h"
"""
