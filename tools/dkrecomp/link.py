"""Static layout and linking of an ExeFS: the job rtld does on the console, done once at build time.

Guest addresses equal host addresses and the lifted C embeds absolute addresses, so every module base is fixed
here and the generator, the images and the runtime all use the same layout. rtld itself is not loaded.

Load order (Horizon's, without rtld): main, subsdk0..9, sdk; each base aligned to 2 MiB, starting at FIRST_BASE,
all below 2^39 (the addresses IPC buffers can encode).

Relocations: R_AARCH64_RELATIVE (base + addend), ABS64 / GLOB_DAT / JUMP_SLOT (symbol + addend). Symbols resolve
through the modules in load order; the first STB_GLOBAL definition wins, then the first STB_WEAK one; an
unresolved reference gets 0 (correct for weak references) and is reported. The lookup order follows ordinary ELF
semantics; that Nintendo's rtld uses the same order is an assumption to confirm.
"""
from __future__ import annotations

import os
import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set, Tuple

from .formats import nso as nso_fmt

FIRST_BASE = 0x8_0000_0000
ALIGN = 0x20_0000
ADDRESS_LIMIT = 1 << 39
ORDER = ["main"] + [f"subsdk{i}" for i in range(10)] + ["sdk"]
R_ABS64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE = 257, 1025, 1026, 1027
DT_INIT, DT_FINI = 12, 13
STB_GLOBAL, STB_WEAK = 1, 2


def align_up(v: int, a: int = ALIGN) -> int:
    return (v + a - 1) & ~(a - 1)


@dataclass
class LinkedModule:
    name: str
    nso: nso_fmt.Nso
    base: int
    size: int                 # image + bss, page aligned
    image: bytearray          # relocated text|ro|data (bss not included)
    init: Optional[int] = None
    fini: Optional[int] = None
    code_pointer_targets: Set[int] = field(default_factory=set)  # addresses in .text stored by relocations
    unresolved: Dict[str, int] = field(default_factory=dict)
    reloc_counts: Dict[str, int] = field(default_factory=dict)

    @property
    def text(self) -> Tuple[int, int]:
        t = self.nso.seg("text")
        return self.base + t.mem_off, self.base + t.mem_off + t.size


def load_layout(exefs: str) -> List[LinkedModule]:
    mods, base = [], FIRST_BASE
    for name in ORDER:
        path = os.path.join(exefs, name)
        if not os.path.exists(path):
            continue
        m = nso_fmt.parse(open(path, "rb").read())
        if not all(s.hash_ok for s in m.segments):
            raise ValueError(f"{path}: segment hash mismatch")
        d = m.seg("data")
        size = (d.mem_off + d.size + m.bss_size + 0xFFF) & ~0xFFF
        lm = LinkedModule(name, m, base, size, bytearray(m.image))
        if DT_INIT in m.dynamic:
            lm.init = base + m.dynamic[DT_INIT][0]
        if DT_FINI in m.dynamic:
            lm.fini = base + m.dynamic[DT_FINI][0]
        mods.append(lm)
        base = align_up(base + size)
    if base > ADDRESS_LIMIT:
        raise ValueError(f"modules end at {base:#x}, above 2^39")
    return mods


def global_symbols(mods: List[LinkedModule]) -> Dict[str, int]:
    strong: Dict[str, int] = {}
    weak: Dict[str, int] = {}
    for lm in mods:
        for s in lm.nso.symbols:
            if not s.defined or not s.name:
                continue
            table = strong if s.bind == STB_GLOBAL else weak if s.bind == STB_WEAK else None
            if table is not None and s.name not in table:
                table[s.name] = lm.base + s.value
    return {**weak, **strong}


def link(mods: List[LinkedModule]) -> Dict[str, int]:
    """Apply every module's relocations in place. Returns the global symbol table."""
    syms = global_symbols(mods)
    for lm in mods:
        m = lm.nso
        counts: Dict[str, int] = {}
        text_lo, text_hi = lm.text
        for tab, tab_size in ((7, 8), (23, 2)):  # DT_RELA/DT_RELASZ, DT_JMPREL/DT_PLTRELSZ
            if tab not in m.dynamic:
                continue
            start, size = m.dynamic[tab][0], m.dynamic[tab_size][0]
            for o in range(start, start + size, 24):
                off, info, addend = struct.unpack_from("<QQq", lm.image, o)
                rtype, sym = info & 0xFFFFFFFF, info >> 32
                if rtype == R_RELATIVE:
                    value = lm.base + addend
                elif rtype in (R_ABS64, R_GLOB_DAT, R_JUMP_SLOT):
                    name = m.symbols[sym].name
                    target = syms.get(name)
                    if target is None:
                        lm.unresolved[name] = lm.unresolved.get(name, 0) + 1
                        target = 0
                    value = (target + addend) if target else 0
                else:
                    raise ValueError(f"{lm.name}: relocation type {rtype} at {off:#x} is not handled")
                struct.pack_into("<Q", lm.image, off, value & 0xFFFFFFFFFFFFFFFF)
                counts[str(rtype)] = counts.get(str(rtype), 0) + 1
                if value:
                    lm_t = next((x for x in mods if x.text[0] <= value < x.text[1]), None)
                    if lm_t is not None:
                        lm_t.code_pointer_targets.add(value)
        lm.reloc_counts = counts
    return syms
