"""Generate C for a whole NSO module and a Ninja build for it.

    python -m dkrecomp.gen user_files/exefs/main --out generated/dkctf_v0/main --base 0x800000000

Function starts: the module's exported FUNC symbols, plus every BL target that falls outside them (PLT stubs and
other unnamed code), which runs until the next known start. The guest addresses baked into the C are
module offset + `--base`, so the runtime must load the module at that base.

Output (all derived from the game: it stays out of version control):
    <out>/f_NNNN.c        lifted functions, `--per-file` per translation unit
    <out>/fn_table.c      the sorted dispatch table (dkr_fn_table)
    <out>/manifest.json   counts, base, unsupported sites, timing
    <out>/build.ninja     compiles everything and links <out>/probe.exe with the runtime
"""
from __future__ import annotations

import argparse
import bisect
import collections
import json
import os
import re
import struct
import sys
import time
from typing import Dict, List, Tuple

from .arm64.decoder import decode
from .coverage import function_ranges
from .formats import nso as nso_fmt
from .recomp.lifter import C_PRELUDE, Function, fname, lift_function

CALL_RE = re.compile(r"\bf_([0-9a-f]+)\(c\)")
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def discover(m: nso_fmt.Nso) -> List[Tuple[int, int, str]]:
    """(start, end, name) offsets: exported functions plus BL targets outside them."""
    t = m.seg("text")
    named = function_ranges(m)
    starts = sorted({s for s, _, _ in named})
    covered = bytearray(t.size // 4 + 1)
    for s, e, _ in named:
        for k in range((s - t.mem_off) // 4, min((e - t.mem_off) // 4, len(covered))):
            covered[k] = 1
    extra = set()
    text = m.image[t.mem_off:t.mem_off + t.size]
    for off in range(0, len(text) - 3, 4):
        w = struct.unpack_from("<I", text, off)[0]
        if w & 0xFC000000 == 0x94000000:  # BL
            imm = w & 0x03FFFFFF
            tgt = t.mem_off + off + ((imm - (1 << 26) if imm & (1 << 25) else imm) << 2)
            if t.mem_off <= tgt < t.mem_off + t.size and not covered[(tgt - t.mem_off) // 4]:
                extra.add(tgt)
    all_starts = sorted(set(starts) | extra)
    out = list(named)
    for s in sorted(extra):
        i = bisect.bisect_right(all_starts, s)
        end = all_starts[i] if i < len(all_starts) else t.mem_off + t.size
        k = (s - t.mem_off) // 4  # stop at the next covered word too
        e = s
        while e < end and not covered[(e - t.mem_off) // 4]:
            e += 4
        out.append((s, e, f"sub_{s:x}"))
    return sorted(out)


def generate(path: str, out: str, base: int, per_file: int) -> Dict:
    t0 = time.time()
    m = nso_fmt.parse(open(path, "rb").read())
    if not all(s.hash_ok for s in m.segments):
        raise SystemExit(f"{path}: segment hash mismatch; refusing to lift a damaged module")
    funcs = discover(m)
    image = m.image
    os.makedirs(out, exist_ok=True)
    starts = {base + s for s, _, _ in funcs}
    is_fn = starts.__contains__
    reasons = collections.Counter()
    files = []
    lifted_words = 0
    for chunk_i in range(0, len(funcs), per_file):
        chunk = funcs[chunk_i:chunk_i + per_file]
        body: List[str] = []
        defined = set()
        for s, e, name in chunk:
            fn = Function(base + s, base + e, name[:80].replace("*/", ""))
            # lift_function reads code bytes relative to `base`, so hand it the image with the base applied.
            lines, unsup = lift_function(image, base, fn, {"is_function": is_fn})
            body += lines
            body.append("")
            defined.add(base + s)
            lifted_words += (e - s) // 4
            for i in unsup:
                reasons[i.f["reason"]] += 1
        called = {int(x, 16) for x in CALL_RE.findall("\n".join(body))} - defined
        name = f"f_{chunk_i // per_file:04d}.c"
        with open(os.path.join(out, name), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(C_PRELUDE)
            fh.write("".join(f"void {fname(a)}(Cpu *c);\n" for a in sorted(called | defined)))
            fh.write("\n".join(body))
        files.append(name)
        print(f"\r  lifted {min(chunk_i + per_file, len(funcs))}/{len(funcs)} functions", end="",
              file=sys.stderr, flush=True)
    print(file=sys.stderr)
    with open(os.path.join(out, "fn_table.c"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(C_PRELUDE + '#include "dkr/rt.h"\n')
        uniq = sorted(starts)
        fh.write("".join(f"void {fname(a)}(Cpu *c);\n" for a in uniq))
        fh.write("const DkrFnEntry dkr_fn_table[] = {\n")
        fh.write("".join(f"    {{0x{a:x}ull, {fname(a)}}},\n" for a in uniq))
        fh.write("};\n")
        fh.write(f"const size_t dkr_fn_count = {len(uniq)};\n")
        fh.write(f"const uint64_t dkr_module_base = 0x{base:x}ull;\n")
    files.append("fn_table.c")
    manifest = {
        "module": os.path.basename(path), "build_id": m.build_id, "base": hex(base),
        "functions": len(funcs), "named": sum(1 for f in funcs if not f[2].startswith("sub_")),
        "bl_target_functions": sum(1 for f in funcs if f[2].startswith("sub_")),
        "lifted_words": lifted_words, "files": len(files), "unsupported_sites": dict(reasons),
        "lift_seconds": round(time.time() - t0, 1),
    }
    with open(os.path.join(out, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=2)
    write_ninja(out, files)
    return manifest


def write_ninja(out: str, files: List[str]) -> None:
    py = sys.executable.replace("\\", "/")
    inc = os.path.join(ROOT, "runtime", "include").replace("\\", "/")
    # Paths in build statements must escape ':' (ninja syntax), e.g. C$:/Users/...
    rt = os.path.join(ROOT, "runtime", "src", "core.c").replace("\\", "/").replace(":", "$:")
    probe = os.path.join(ROOT, "runtime", "src", "probe_main.c").replace("\\", "/").replace(":", "$:")
    lines = [
        "# Generated by dkrecomp.gen. Compiles the lifted module with Zig's Clang (see docs/TESTING.md).",
        f"cc = {py} -m ziglang cc -target x86_64-windows-gnu",
        "cflags = -O1 -std=c11 -mfma -ffp-contract=off -fno-math-errno -w",
        f"inc = -I{inc}",
        "rule cc",
        "  command = $cc $cflags $inc -c $in -o $out",
        "  description = CC $out",
        "rule link",
        "  command = $cc $in -o $out",
        "  description = LINK $out",
    ]
    objs = []
    for f in files:
        o = f"obj/{f[:-2]}.o"
        objs.append(o)
        lines.append(f"build {o}: cc {f}")
    lines.append(f"build obj/core.o: cc {rt}")
    lines.append(f"build obj/probe_main.o: cc {probe}")
    lines.append(f"build probe.exe: link {' '.join(objs)} obj/core.o obj/probe_main.o")
    lines.append("default probe.exe")
    with open(os.path.join(out, "build.ninja"), "w", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("nso")
    ap.add_argument("--out", required=True)
    ap.add_argument("--base", type=lambda v: int(v, 0), default=0x800000000)
    ap.add_argument("--per-file", type=int, default=600)
    a = ap.parse_args(argv)
    print(json.dumps(generate(a.nso, a.out, a.base, a.per_file), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
