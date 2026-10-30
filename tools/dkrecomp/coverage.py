"""Measure how much of an NSO's code the DKRecomp decoder can lift.

    python -m dkrecomp.coverage user_files/exefs [--out reports/coverage]

Functions come from the module's exported FUNC symbols (start and size). Every instruction word inside a function
is decoded; the report counts supported and unsupported words, groups the unsupported ones by decoder reason and by
Capstone mnemonic (when Capstone is installed), and counts functions that are fully supported. Words outside any
exported function are reported separately: they are code the symbols do not cover (or data).
The report holds counts and mnemonics only, no game code.
"""
from __future__ import annotations

import argparse
import collections
import json
import os
import struct
import sys
from typing import Dict, List, Tuple

from .arm64.decoder import decode
from .formats import nso as nso_fmt


def function_ranges(m: nso_fmt.Nso) -> List[Tuple[int, int, str]]:
    """(start, end, name) of exported functions in .text, one per start address (the largest size wins)."""
    t = m.seg("text")
    best: Dict[int, Tuple[int, str]] = {}
    for s in m.symbols:
        if s.defined and s.type == 2 and s.size and t.mem_off <= s.value < t.mem_off + t.size:
            if s.value not in best or s.size > best[s.value][0]:
                best[s.value] = (s.size, s.name)
    return sorted((a, a + n, name) for a, (n, name) in best.items())


def analyze(path: str, with_capstone: bool = True) -> Dict:
    m = nso_fmt.parse(open(path, "rb").read())
    t = m.seg("text")
    text = m.image[t.mem_off:t.mem_off + t.size]
    md = None
    if with_capstone:
        try:
            import capstone
            md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        except ImportError:
            md = None
    funcs = function_ranges(m)
    covered = bytearray(len(text) // 4 + 1)
    ops = collections.Counter()
    reasons = collections.Counter()
    mnems = collections.Counter()
    words = supported = 0
    fully = 0
    blocking = collections.Counter()  # mnemonic -> number of functions it blocks
    for start, end, _name in funcs:
        ok = True
        fn_mnems = set()
        for a in range(start, min(end, t.mem_off + t.size), 4):
            k = (a - t.mem_off) // 4
            if covered[k]:
                continue  # aliases / overlapping symbols: count each word once
            covered[k] = 1
            w = struct.unpack_from("<I", text, a - t.mem_off)[0]
            i = decode(w, a)
            words += 1
            ops[i.op] += 1
            if i.op == "unsupported":
                ok = False
                reasons[i.f["reason"]] += 1
                if md is not None:
                    ci = next(md.disasm(w.to_bytes(4, "little"), a), None)
                    mn = ci.mnemonic if ci else "(invalid)"
                    mnems[mn] += 1
                    fn_mnems.add(mn)
            else:
                supported += 1
        if ok:
            fully += 1
        for mn in fn_mnems:
            blocking[mn] += 1
    outside = sum(1 for k in range(len(text) // 4) if not covered[k])
    return {
        "module": os.path.basename(path),
        "text_words": len(text) // 4,
        "functions": len(funcs),
        "functions_fully_supported": fully,
        "words_in_functions": words,
        "words_supported": supported,
        "words_outside_functions": outside,
        "unsupported_by_reason": dict(reasons.most_common()),
        "unsupported_by_mnemonic": dict(mnems.most_common()),
        "functions_blocked_by_mnemonic": dict(blocking.most_common()),
        "ops": dict(ops.most_common()),
    }


def to_markdown(reports: List[Dict]) -> str:
    out = ["# DKRecomp lifter coverage", "",
           "Words = 32-bit instruction words inside exported functions. Counts only; no game code.", "",
           "| Module | Functions | Fully supported | Words | Supported | Outside functions |",
           "|---|---|---|---|---|---|"]
    for r in reports:
        pct = 100 * r["words_supported"] / max(r["words_in_functions"], 1)
        fpct = 100 * r["functions_fully_supported"] / max(r["functions"], 1)
        out.append(f"| `{r['module']}` | {r['functions']} | {r['functions_fully_supported']} ({fpct:.1f}%) | "
                   f"{r['words_in_functions']} | {r['words_supported']} ({pct:.2f}%) | "
                   f"{r['words_outside_functions']} |")
    for r in reports:
        out += ["", f"## {r['module']}", "", "Unsupported by decoder reason:", ""]
        out += [f"- {k}: {v}" for k, v in r["unsupported_by_reason"].items()]
        if r["unsupported_by_mnemonic"]:
            out += ["", "Top unsupported mnemonics (words / functions they block):", "",
                    "| Mnemonic | Words | Functions blocked |", "|---|---|---|"]
            for mn, n in list(r["unsupported_by_mnemonic"].items())[:40]:
                out.append(f"| `{mn}` | {n} | {r['functions_blocked_by_mnemonic'].get(mn, 0)} |")
    return "\n".join(out) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exefs", help="extracted ExeFS folder or a single NSO")
    ap.add_argument("--out", default="reports/coverage")
    a = ap.parse_args(argv)
    paths = [a.exefs] if os.path.isfile(a.exefs) else [
        os.path.join(a.exefs, n) for n in sorted(os.listdir(a.exefs))
        if not n.endswith(".npdm") and open(os.path.join(a.exefs, n), "rb").read(4) == b"NSO0"]
    reports = [analyze(p) for p in paths]
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out + ".json", "w", encoding="utf-8") as f:
        json.dump(reports, f, indent=2)
    md = to_markdown(reports)
    with open(a.out + ".md", "w", encoding="utf-8") as f:
        f.write(md)
    print(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
