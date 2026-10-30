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


TERMINATORS = {"b", "br", "ret", "brk"}


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
