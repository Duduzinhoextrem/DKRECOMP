"""Map NVN proc names to the original routines of the NVN driver linked into the target's sdk module.

The SDK's nvnDeviceGetProcAddress binary-searches a sorted pool of names stored without the "nvn" prefix
(sdk+0x2e01e0) and indexes a function table that sdk+0x2dec58 installs from a static array (GOT entry
sdk+0x99b808). Reading that array gives the exact routine behind every name, so layouts of builders and objects
can be measured from the game's own driver instead of assumed from newer NVN versions.

Usage: python -m dkrecomp.nvnmap EXEFS [--list] [--dis NAME_REGEX] [--max N]
Output is analysis of the user's files: print it, keep it under generated/, never commit it.
"""
import argparse
import re
import struct
import sys

from . import link

# Offsets in the sdk of the 4.4.0 ExeFS (build checked by its own name table: 477 entries, sorted).
NAME_BASE = 0x818360      # string pool the offsets are relative to
NAME_OFFSETS = 0x81B5D0   # u32 offset per name, sorted order
TABLE_GOT = 0x99B808      # GOT entry holding the static function table
COUNT = 477
# Tables in the data segment that behave as constants. The texture format table (0x34 bytes per NVNformat) is
# reached only through GOT slot sdk+0x99b7f8: no code addresses its pages directly and none of the 42 loads of that
# slot is followed by a store through it (checked over the whole text segment).
CONSTANT_TABLES = (0x99F2B0,)


def load(exefs):
    mods = link.load_layout(exefs)
    link.link(mods)
    sdk = next(m for m in mods if m.name == "sdk")
    return mods, sdk


def proc_map(sdk):
    img = sdk.image
    q = lambda off: struct.unpack_from("<Q", img, off)[0]
    table = q(TABLE_GOT) - sdk.base
    out = {}
    prev = ""
    for i in range(COUNT):
        o = NAME_BASE + struct.unpack_from("<I", img, NAME_OFFSETS + 4 * i)[0]
        name = img[o:img.index(b"\0", o)].decode("ascii")
        if name <= prev:
            raise ValueError(f"name table not sorted at {i}: {prev} >= {name}; wrong sdk build?")
        prev = name
        f = q(table + 8 * i)
        out["nvn" + name] = f - sdk.base if f else None
    return out


def relocated_slots(sdk):
    """Offsets of 8-byte slots the dynamic relocations write (GOT entries, vtables): constant once linked."""
    m, slots = sdk.nso, set()
    for tab, tab_size in ((7, 8), (23, 2)):
        if tab in m.dynamic:
            start, size = m.dynamic[tab][0], m.dynamic[tab_size][0]
            slots.update(struct.unpack_from("<Q", m.image, o)[0] for o in range(start, start + size, 24))
    return slots


class Purity:
    """Decides whether a routine only works on its arguments and constant data.

    Every register carries a provenance: "arg" (x0..x7, sp, x29 and anything computed from them), "const" (an
    address from adrp into text/ro or from a relocated slot pointing there), "loaded" (a value read from memory, or
    anything a call returns) or "scalar" (immediates). A routine is pure when, on every path:
      - loads go through "arg" or "const" bases and stores through "arg" bases; a pointer read from memory is never
        followed, so builder->device->... makes a routine impure;
      - calls go to pure routines and pass no "loaded" value in x0..x7;
      - there is no indirect branch or call (jump tables included), SVC, system register or atomic access.
    Such a routine computes the same thing whether or not the rest of the original driver ran, so the recompiled
    original can serve the game directly. The analysis is conservative: it may call a pure routine impure."""

    LOADS = ("ldr", "ldp", "ldur", "ldnp")
    STORES = ("str", "stp", "stur", "stnp")
    ATOMIC = ("ldx", "ldax", "stx", "stlx", "ldar", "stlr", "cas", "ldadd", "ldclr", "ldeor", "ldset", "swp")

    def __init__(self, sdk):
        import capstone
        self.sdk, self.memo, self.cs = sdk, {}, capstone
        self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.md.detail = True
        ro, text = sdk.nso.seg("ro"), sdk.nso.seg("text")
        self.const = (text.mem_off, ro.mem_off + ro.size)  # text and ro are adjacent
        self.slots = relocated_slots(sdk)

    def _insn(self, a):
        return next(self.md.disasm(bytes(self.sdk.image[a:a + 4]), a), None)

    def _const_slot(self, off):
        if off not in self.slots:
            return False
        v = struct.unpack_from("<Q", self.sdk.image, off)[0] - self.sdk.base
        return self.const[0] <= v < self.const[1] or v in CONSTANT_TABLES

    def __call__(self, off):
        if off in self.memo:
            return self.memo[off]
        self.memo[off] = "pure"  # recursion
        why = self._walk(off)
        self.memo[off] = why
        return why

    @staticmethod
    def _norm(name):
        """x/w register -> "xN", sp -> "sp"; zero and SIMD registers -> None (no address)."""
        if name in ("sp", "wsp"):
            return "sp"
        if name[0] in "xw" and name[1:].isdigit():
            return "x" + name[1:]
        return None

    @staticmethod
    def _combine(kinds):
        for k in ("page", "const", "arg", "loaded"):
            if k in kinds:
                return k
        return "scalar"

    def _walk(self, off):
        arm = self.cs.arm64
        entry = {f"x{n}": "arg" for n in range(8)}
        entry.update(sp="arg", x29="arg")
        work, seen = [(off, entry, {})], set()
        while work:
            a, kind, pages = work.pop()
            while True:
                key = (a, tuple(sorted(kind.items())))
                if key in seen:
                    break
                seen.add(key)
                if len(seen) > 20000:
                    return "too many paths"
                i = self._insn(a)
                if i is None:
                    return f"undecodable at {a:#x}"
                m = i.mnemonic
                if m in ("blr", "br", "svc", "adr", "mrs", "msr", "hlt", "brk") or m.startswith(self.ATOMIC):
                    return f"{m} at {a:#x}"
                regs = [self._norm(i.reg_name(o.reg)) if o.type == arm.ARM64_OP_REG else None for o in i.operands]
                mem = [o.mem for o in i.operands if o.type == arm.ARM64_OP_MEM]
                k = lambda r: kind.get(r, "loaded") if r else "scalar"
                if m == "adrp":
                    page = i.operands[1].imm
                    kind[regs[0]] = "const" if self.const[0] <= page < self.const[1] else "page"
                    pages[regs[0]] = page
                elif m.startswith(self.LOADS) and mem:
                    base = self._norm(i.reg_name(mem[0].base))
                    bk = k(base)
                    if bk == "page":
                        slot = pages[base] + mem[0].disp
                        if not self._const_slot(slot):
                            return f"reads writable {slot:#x} at {a:#x}"
                        val = "const"
                    elif bk in ("arg", "const"):
                        val = "loaded"
                    else:
                        return f"follows a {bk} pointer at {a:#x}"
                    for r in regs:
                        if r:
                            kind[r] = val
                    if i.writeback and base:
                        kind[base] = bk
                elif m.startswith(self.STORES) and mem:
                    base = self._norm(i.reg_name(mem[0].base))
                    if k(base) != "arg":
                        return f"stores through a {k(base)} pointer at {a:#x}"
                    if i.writeback and base:
                        kind[base] = "arg"
                elif m == "bl":
                    t = i.operands[0].imm
                    bad = [f"x{n}" for n in range(8) if kind.get(f"x{n}") in ("loaded", "page")]
                    if bad:  # may be a plain number, but nothing here proves the callee will not dereference it
                        return f"passes loaded {bad[0]} to {t:#x} at {a:#x}"
                    r = self(t)
                    if r != "pure":
                        return f"calls {t:#x} ({r})"
                    for n in range(19):
                        kind[f"x{n}"] = "loaded"
                elif m == "ret":
                    break
                elif m == "b":
                    a = i.operands[0].imm
                    continue
                elif m.startswith(("b.", "cb", "tb")):
                    work.append((i.operands[-1].imm, dict(kind), dict(pages)))
                elif regs and regs[0]:
                    dst, srcs = regs[0], [r for r in regs[1:] if r]
                    simd_src = any(o.type == arm.ARM64_OP_REG and self._norm(i.reg_name(o.reg)) is None
                                   and i.reg_name(o.reg)[0] in "vqdshb" for o in i.operands[1:])
                    if m == "movk":
                        pass
                    elif simd_src:
                        kind[dst] = "loaded"
                    else:
                        ks = [k(r) for r in srcs]
                        if "page" in ks and m != "add":
                            return f"computes with a writable page address at {a:#x}"
                        kind[dst] = self._combine(ks)
                        if m in ("mov", "add") and srcs and srcs[0] in pages:
                            pages[dst] = pages[srcs[0]]
                            if m == "add":  # adrp+add: an address inside the page, writable unless const
                                kind[dst] = "const" if self.const[0] <= pages[dst] < self.const[1] else "loaded-page"
                a += 4
        return "pure"


def disassemble(sdk, off, limit):
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    lines = []
    for i in md.disasm(bytes(sdk.image[off:off + 4 * limit]), off):
        lines.append(f"  sdk+{i.address:#x}: {i.mnemonic} {i.op_str}")
        if i.mnemonic in ("ret", "b", "br"):
            break
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("exefs")
    ap.add_argument("--list", action="store_true", help="print name and sdk offset of every proc")
    ap.add_argument("--dis", help="disassemble the procs whose name matches this regex")
    ap.add_argument("--max", type=int, default=40, help="instructions per routine (stops at ret/b/br)")
    ap.add_argument("--purity", action="store_true", help="print name, sdk offset and purity verdict of every proc")
    a = ap.parse_args(argv)
    _, sdk = load(a.exefs)
    procs = proc_map(sdk)
    if a.list:
        for n, off in procs.items():
            print(f"{n}\t{off:#x}" if off is not None else f"{n}\t-")
    if a.purity:
        pure = Purity(sdk)
        for n, off in procs.items():
            print(f"{n}\t{off:#x}\t{pure(off)}" if off is not None else f"{n}\t-\tno routine")
    if a.dis:
        rx = re.compile(a.dis)
        for n, off in procs.items():
            if rx.search(n) and off is not None:
                print(f"{n} (sdk+{off:#x})")
                print("\n".join(disassemble(sdk, off, a.max)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
