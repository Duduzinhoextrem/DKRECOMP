"""Lift a whole module: a set of guest functions to one C translation unit plus the dispatch table."""
from __future__ import annotations

import collections
from dataclasses import dataclass, field
from typing import Dict, List

from .lifter import C_PRELUDE, Function, fname, lift_function


@dataclass
class ModuleResult:
    c_source: str
    functions: List[Function]
    unsupported: Dict[str, int] = field(default_factory=dict)  # reason -> count
    unsupported_sites: List[int] = field(default_factory=list)


def lift_module(code: bytes, base: int, functions: List[Function]) -> ModuleResult:
    functions = sorted(functions, key=lambda f: f.entry)
    starts = {f.entry for f in functions}
    out = [C_PRELUDE, '#include "dkr/rt.h"', ""]
    out += [f"void {fname(f.entry)}(Cpu *c);" for f in functions]
    out.append("")
    reasons = collections.Counter()
    sites = []
    for fn in functions:
        lines, unsup = lift_function(code, base, fn, {"is_function": starts.__contains__})
        out += lines
        out.append("")
        for i in unsup:
            reasons[i.f["reason"]] += 1
            sites.append(i.addr)
    out.append("const DkrFnEntry dkr_fn_table[] = {")
    out += [f"    {{0x{f.entry:x}ull, {fname(f.entry)}}}," for f in functions]
    out.append("};")
    out.append(f"const size_t dkr_fn_count = {len(functions)};")
    return ModuleResult("\n".join(out) + "\n", functions, dict(reasons), sites)
