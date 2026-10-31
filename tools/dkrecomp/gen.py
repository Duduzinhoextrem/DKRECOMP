"""Generate C for a whole ExeFS, the linked module images and a Ninja build of the native program.

    python -m dkrecomp.gen user_files/exefs --out generated/dkctf_v0

Layout and linking come from dkrecomp.link (rtld's job, done at build time). For each module (main, subsdk*,
sdk) the function starts are its exported FUNC symbols, every BL target outside them, and every address in its
.text that a relocation stores (vtables, callbacks); unnamed starts run until the next known start or the end of
the function that contains them.

Output (all derived from the game, so it stays out of version control):
    <out>/<module>/f_NNNN.c   lifted functions
    <out>/<module>/table.c    dkr_fn_table_<module>, sorted by address
    <out>/<module>.img        the linked module image (text|ro|data, relocations applied; bss not stored)
    <out>/layout.c            module descriptors and the boot symbols for the runtime
    <out>/manifest.json       counts and timings
    <out>/build.ninja         compiles everything and links <out>/dkctf.exe with the runtime
Files whose content did not change are not rewritten, so Ninja only recompiles what changed.
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
from typing import Dict, List, Set, Tuple

from . import link as linker
from .coverage import function_ranges
from .formats import nso as nso_fmt
from .recomp.lifter import C_PRELUDE, Function, fname, lift_function

CALL_RE = re.compile(r"\bf_([0-9a-f]+)\(c\)")
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BOOT_SYMBOLS = {"init_start": "_ZN2nn4init5StartEmmPFvvES2_", "nn_main": "nnMain"}
RUNTIME_SOURCES = ["core.c", "loader.c", "horizon.c", "ipc.c", "services.c", "kernel.c", "fs.c", "save.c"]
GFX_OVERRIDES = {
    "_ZN2nv18InitializeGraphicsEPvm": "dkr_nvn_graphics_init",
    "_ZN2nv16FinalizeGraphicsEv": "dkr_nvn_graphics_finalize",
    "_ZN2nv26InitializeGraphicsDevtoolsEPvm": "dkr_nvn_devtools_init",
    "_ZN2nv24FinalizeGraphicsDevtoolsEv": "dkr_nvn_devtools_finalize",
    "nvnBootstrapLoader": "dkr_nvn_bootstrap",
}


def write_if_changed(path: str, text: str) -> bool:
    data = text.encode("utf-8")
    if os.path.exists(path) and open(path, "rb").read() == data:
        return False
    with open(path, "wb") as fh:
        fh.write(data)
    return True


def _sx(v: int, n: int) -> int:
    return v - (1 << n) if v & (1 << (n - 1)) else v


def branch_target(w: int, pc: int):
    """Target of a direct branch (B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ), or None."""
    if w & 0x7C000000 == 0x14000000:
        return pc + (_sx(w & 0x3FFFFFF, 26) << 2)
    if w & 0xFF000010 == 0x54000000 or w & 0x7E000000 == 0x34000000:
        return pc + (_sx((w >> 5) & 0x7FFFF, 19) << 2)
    if w & 0x7E000000 == 0x36000000:
        return pc + (_sx((w >> 5) & 0x3FFF, 14) << 2)
    return None


def materialised_code_addresses(text: bytes, t_lo: int, t_hi: int) -> Set[int]:
    """Code addresses built with ADR, or ADRP followed (within 4 instructions) by ADD of the same register:
    position-independent code passes function pointers this way, without a relocation."""
    out = set()
    words = struct.unpack(f"<{len(text) // 4}I", text[:len(text) & ~3])
    for k, w in enumerate(words):
        pc = t_lo + 4 * k
        if w & 0x9F000000 == 0x10000000:  # ADR
            tgt = pc + _sx(((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3, 21)
        elif w & 0x9F000000 == 0x90000000:  # ADRP
            page = (pc & ~0xFFF) + (_sx(((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3, 21) << 12)
            rd, tgt = w & 31, None
            for j in range(k + 1, min(k + 5, len(words))):
                a = words[j]
                if a & 0xFFC00000 == 0x91000000 and (a >> 5) & 31 == rd:  # ADD Xd, Xrd, #imm12 (no shift)
                    tgt = page + ((a >> 10) & 0xFFF)
                    break
            if tgt is None:
                continue
        else:
            continue
        if t_lo <= tgt < t_hi and tgt % 4 == 0:
            out.add(tgt)
    return out


def unwind_code_addresses(m: nso_fmt.Nso) -> Set[int]:
    """Function starts in the standard .eh_frame_hdr search table (DW_EH_PE_datarel | sdata4).

    Nintendo's modules use version 1, a PC-relative sdata4 EH-frame pointer, a udata4 count and pairs of
    header-relative sdata4 addresses. Unwind metadata also covers callbacks which no BL or nearby ADR finds.
    """
    if m.eh_frame_hdr is None:
        return set()
    lo, hi = m.eh_frame_hdr
    if lo < 0 or hi > len(m.image) or hi - lo < 12:
        raise nso_fmt.NsoError("truncated .eh_frame_hdr")
    if m.image[lo:lo + 4] != bytes((1, 0x1B, 3, 0x3B)):
        raise nso_fmt.NsoError("unsupported .eh_frame_hdr encoding")
    count, = struct.unpack_from("<I", m.image, lo + 8)
    if count > (hi - lo - 12) // 8:
        raise nso_fmt.NsoError("truncated .eh_frame_hdr search table")
    text = m.seg("text")
    end = text.mem_off + text.size
    return {lo + relative for relative, _fde in struct.iter_unpack("<ii", m.image[lo + 12:lo + 12 + count * 8])
            if text.mem_off <= lo + relative < end and (lo + relative) % 4 == 0}


def discover(m: nso_fmt.Nso, extra_starts: Set[int]) -> List[Tuple[int, int, str]]:
    """(start, end, name) as module offsets.

    Starts: exported functions, `extra_starts` (relocated code pointers), unwind metadata, ADR/ADRP+ADD,
    and, to a fixpoint, every direct branch target that lies outside the function the branch is in. After the
    fixpoint each direct branch lands inside its own function or on a known start, so the lifter never needs
    rt_dispatch for a direct branch. Unnamed functions run to the next start (or the next named function);
    a start inside a named function is lifted from there to that function's end."""
    t = m.seg("text")
    t_lo, t_hi = t.mem_off, t.mem_off + t.size
    text = m.image[t_lo:t_hi]
    words = struct.unpack(f"<{len(text) // 4}I", text[:len(text) & ~3])
    named = function_ranges(m)
    named_starts = {s for s, _, _ in named}
    covered = bytearray(len(words) + 1)
    container: Dict[int, int] = {}  # word index -> end of the named function covering it
    for s, e, _ in named:
        for k in range((s - t_lo) // 4, min((e - t_lo) // 4, len(words))):
            covered[k] = 1
            container[k] = max(container.get(k, 0), e)
    extra = ({a for a in extra_starts if t_lo <= a < t_hi and a % 4 == 0} | unwind_code_addresses(m) |
             materialised_code_addresses(text, t_lo, t_hi)) - named_starts

    def ranges():
        all_starts = sorted(named_starts | extra)
        out = list(named)
        for s in sorted(extra):
            k = (s - t_lo) // 4
            if covered[k]:
                e = container[k]
            else:
                i = bisect.bisect_right(all_starts, s)
                end = all_starts[i] if i < len(all_starts) else t_hi
                e = s
                while e < end and not covered[(e - t_lo) // 4]:
                    e += 4
            out.append((s, e, f"sub_{s:x}"))
        return out

    scanned: Dict[int, int] = {}  # start -> end of the range last scanned
    while True:
        todo = [(s, e) for s, e, _ in ranges() if scanned.get(s) != e]  # new functions, or ends that moved
        if not todo:
            break
        new = set()
        for s, e in todo:
            scanned[s] = e
            for a in range(s, min(e, t_hi), 4):
                w = words[(a - t_lo) // 4]
                tgt = branch_target(w, a)
                if tgt is None or not t_lo <= tgt < t_hi or tgt in named_starts or tgt in extra:
                    continue
                # A BL always needs a function at its target, even inside the caller's range (static helpers
                # placed inside a larger unnamed range); other branches only when they leave the function.
                if w & 0xFC000000 == 0x94000000 or not (s <= tgt < e):
                    new.add(tgt)
        extra |= new
    return sorted(ranges())


def partition_functions(funcs: List[Tuple[int, int, str]], out_dir: str, per_file: int, base: int = 0):
    """Keep chunk boundaries stable when discovery adds functions, so unrelated files do not all rebuild."""
    if per_file <= 0:
        raise ValueError("per_file must be positive")
    if not funcs:
        return []
    layout_path = os.path.join(out_dir, "chunks.json")
    anchors = []
    if os.path.exists(layout_path):
        with open(layout_path, encoding="utf-8") as stream:
            layout = json.load(stream)
        if layout.get("per_file") == per_file:
            anchors = layout["anchors"]
    else:
        # Migrate already-generated files without moving their existing functions into different chunks.
        for name in sorted(os.listdir(out_dir)):
            if not re.fullmatch(r"f_[0-9]{4}\.c", name):
                continue
            with open(os.path.join(out_dir, name), encoding="utf-8") as stream:
                for line in stream:
                    match = re.match(r"void f_([0-9a-f]+)\(Cpu \*c\) \{", line)
                    if match:
                        # Generated C uses absolute addresses; functions here use module offsets.
                        anchors.append(int(match.group(1), 16))
                        break
        if anchors:
            anchors = [a - base for a in anchors]
    if not anchors:
        anchors = [s for s, _e, _name in funcs[::per_file]]
    if anchors != sorted(set(anchors)):
        raise ValueError("invalid generated chunk layout")
    chunks = [[] for _ in anchors]
    for fn in funcs:
        chunks[max(0, bisect.bisect_right(anchors, fn[0]) - 1)].append(fn)
    write_if_changed(layout_path, json.dumps({"per_file": per_file, "anchors": anchors}, indent=2) + "\n")
    return chunks


def lift_module(lm: linker.LinkedModule, out_dir: str, per_file: int, overrides=None) -> Dict:
    t0 = time.time()
    m = lm.nso
    overrides = overrides or {}
    override_sites = {}
    for symbol in m.symbols:
        if symbol.defined and symbol.type == 2 and symbol.name in overrides:
            host = overrides[symbol.name]
            previous = override_sites.get(symbol.value)
            if previous and previous != host:
                raise ValueError(f"conflicting host overrides at {symbol.value:#x}")
            override_sites[symbol.value] = host
    seeds = {a - lm.base for a in lm.code_pointer_targets}
    funcs = discover(m, seeds | set(override_sites))
    os.makedirs(out_dir, exist_ok=True)
    base = lm.base
    starts = {base + s for s, _, _ in funcs}
    is_fn = starts.__contains__
    reasons = collections.Counter()
    files, written, lifted_words = [], 0, 0
    overridden = []
    image = bytes(lm.nso.image)  # code is never relocated, so the unrelocated image serves for lifting
    processed = 0
    for chunk_i, chunk in enumerate(partition_functions(funcs, out_dir, per_file, base)):
        body: List[str] = []
        defined = set()
        for s, e, name in chunk:
            fn = Function(base + s, base + e, name[:80].replace("*/", ""))
            host = override_sites.get(s)
            if host:
                if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", host):
                    raise ValueError(f"invalid host override identifier: {host}")
                # Replace the lifted body itself: both C direct calls and table dispatch reach the override.
                lines = [f"void {fname(base + s)}(Cpu *c) {{ /* {fn.name} */",
                         f"    extern void {host}(Cpu *);", f"    {host}(c);", "}"]
                unsup = []
                overridden.append({"name": name, "address": hex(base + s), "host": host, "guest_words": (e - s) // 4})
            else:
                lines, unsup = lift_function(image, base, fn, {"is_function": is_fn})
                lifted_words += (e - s) // 4
            if name == BOOT_SYMBOLS["nn_main"]:
                # Direct calls bypass rt_dispatch, so report the application entry inside its lifted body.
                lines[1:1] = ["    extern void dkr_boot_entry(Cpu *, uint64_t);",
                              f"    dkr_boot_entry(c, 0x{base + s:x}ull);"]
            body += lines
            body.append("")
            defined.add(base + s)
            for i in unsup:
                reasons[i.f["reason"]] += 1
        called = {int(x, 16) for x in CALL_RE.findall("\n".join(body))} - defined
        name = f"f_{chunk_i:04d}.c"
        text = C_PRELUDE + "".join(f"void {fname(a)}(Cpu *c);\n" for a in sorted(called | defined)) + "\n".join(body)
        written += write_if_changed(os.path.join(out_dir, name), text)
        files.append(name)
        processed += len(chunk)
        print(f"\r  {lm.name}: lifted {processed}/{len(funcs)} functions", end="",
              file=sys.stderr, flush=True)
    print(file=sys.stderr)
    uniq = sorted(starts)
    table = (C_PRELUDE + '#include "dkr/rt.h"\n' + "".join(f"void {fname(a)}(Cpu *c);\n" for a in uniq) +
             f"const DkrFnEntry dkr_fn_table_{lm.name}[] = {{\n" +
             "".join(f"    {{0x{a:x}ull, {fname(a)}}},\n" for a in uniq) + "};\n" +
             f"const size_t dkr_fn_count_{lm.name} = {len(uniq)};\n")
    written += write_if_changed(os.path.join(out_dir, "table.c"), table)
    files.append("table.c")
    return {"module": lm.name, "base": hex(base), "size": hex(lm.size), "functions": len(funcs),
            "named": sum(1 for f in funcs if not f[2].startswith("sub_")),
            "discovered": sum(1 for f in funcs if f[2].startswith("sub_")), "lifted_words": lifted_words,
            "files": files, "files_rewritten": written, "unsupported_sites": dict(reasons), "host_overrides": overridden,
            "unresolved_symbols": lm.unresolved, "relocations": lm.reloc_counts,
            "lift_seconds": round(time.time() - t0, 1)}


def write_layout(mods: List[linker.LinkedModule], syms: Dict[str, int], npdm_stack: int, out: str) -> None:
    lines = [C_PRELUDE, '#include "dkr/rt.h"', ""]
    for lm in mods:
        lines.append(f"extern const DkrFnEntry dkr_fn_table_{lm.name}[];")
        lines.append(f"extern const size_t dkr_fn_count_{lm.name};")
    lines.append("const DkrModule dkr_modules[] = {")
    for lm in mods:
        t_lo, t_hi = lm.text
        lines.append(f'    {{"{lm.name}", 0x{lm.base:x}ull, 0x{lm.size:x}ull, 0x{len(lm.image):x}ull, '
                     f'0x{t_lo:x}ull, 0x{t_hi:x}ull, 0x{lm.init or 0:x}ull, dkr_fn_table_{lm.name}, '
                     f'&dkr_fn_count_{lm.name}}},')
    lines.append("};")
    lines.append(f"const size_t dkr_module_count = {len(mods)};")
    for key, sym in BOOT_SYMBOLS.items():
        if sym not in syms:
            raise SystemExit(f"boot symbol {sym} is not exported by any module")
        lines.append(f"const uint64_t dkr_sym_{key} = 0x{syms[sym]:x}ull; /* {sym} */")
    lines.append(f"const uint64_t dkr_main_thread_stack_size = 0x{npdm_stack:x}ull; /* main.npdm */")
    write_if_changed(os.path.join(out, "layout.c"), "\n".join(lines) + "\n")


def write_ninja(out: str, modules: List[Dict], gfx: bool = False) -> None:
    py = sys.executable.replace("\\", "/")
    inc = os.path.join(ROOT, "runtime", "include").replace("\\", "/")
    esc = lambda p: p.replace("\\", "/").replace(":", "$:")  # ':' must be escaped in build statements
    lines = [
        "# Generated by dkrecomp.gen. Compiles the lifted modules with Zig's Clang (see docs/TESTING.md).",
        f"cc = {py} -m ziglang cc -target x86_64-windows-gnu",
        "cflags = -O1 -std=c11 -mfma -ffp-contract=off -fno-math-errno -w",
        f"inc = -I{inc}",
        f"runtime_flags = {'-DDKR_ENABLE_GFX' if gfx else ''}",
        f"gfx_inc = -I{os.path.join(ROOT, 'toolchain', 'vulkan-headers', 'include').replace(chr(92), '/')}",
        f"libs = {'-luser32 -lgdi32' if gfx else ''}",
        "rule cc",
        "  command = $cc $cflags $inc -MMD -MF $out.d -c $in -o $out",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = CC $out",
        "rule cc_runtime",
        "  command = $cc $cflags $inc $runtime_flags -MMD -MF $out.d -c $in -o $out",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = CC $out",
        "rule cc_gfx",
        "  command = $cc $cflags $inc $gfx_inc -MMD -MF $out.d -c $in -o $out",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = CC $out",
        "rule link",
        # 16 MiB host stack for the process's main thread, which runs the guest main thread (lifted code nests as
        # deep as guest calls); threads the guest creates get 8 MiB in kernel.c.
        "  command = $cc $in -o $out -Wl,--stack,16777216 $libs",
        "  description = LINK $out",
    ]
    objs = []
    for mod in modules:
        for f in mod["files"]:
            o = f"obj/{mod['module']}/{f[:-2]}.o"
            objs.append(o)
            lines.append(f"build {o}: cc {mod['module']}/{f}")
    for src in RUNTIME_SOURCES:
        o = f"obj/rt/{src[:-2]}.o"
        objs.append(o)
        lines.append(f"build {o}: cc_runtime {esc(os.path.join(ROOT, 'runtime', 'src', src))}")
    if gfx:
        for src in ("vk.c", "nvn.c"):
            o = f"obj/gfx/{src[:-2]}.o"
            objs.append(o)
            lines.append(f"build {o}: cc_gfx {esc(os.path.join(ROOT, 'runtime', 'gfx', src))}")
    objs.append("obj/layout.o")
    lines.append("build obj/layout.o: cc layout.c")
    lines.append(f"build dkctf.exe: link {' '.join(objs)}")
    lines.append("default dkctf.exe")
    write_if_changed(os.path.join(out, "build.ninja"), "\n".join(lines) + "\n")


def generate(exefs: str, out: str, per_file: int, gfx: bool = False) -> Dict:
    from .formats import npdm
    t0 = time.time()
    mods = linker.load_layout(exefs)
    syms = linker.link(mods)
    if gfx:
        if not os.path.isfile(os.path.join(ROOT, "toolchain", "vulkan-headers", "include", "vulkan", "vulkan.h")):
            raise SystemExit("run .venv/Scripts/python tools/fetch_gfx_deps.py before --gfx")
        for symbol in ("_ZN2nv18InitializeGraphicsEPvm", "nvnBootstrapLoader"):
            if symbol not in syms:
                raise SystemExit(f"--gfx requires exported SDK symbol {symbol}")
    os.makedirs(out, exist_ok=True)
    stack = npdm.parse(open(os.path.join(exefs, "main.npdm"), "rb").read()).main_thread_stack_size
    results = []
    for lm in mods:
        results.append(lift_module(lm, os.path.join(out, lm.name), per_file, GFX_OVERRIDES if gfx else None))
        img = os.path.join(out, f"{lm.name}.img")
        if not os.path.exists(img) or open(img, "rb").read() != bytes(lm.image):
            with open(img, "wb") as fh:
                fh.write(lm.image)
    write_layout(mods, syms, stack, out)
    write_ninja(out, results, gfx)
    manifest = {"modules": [{k: v for k, v in r.items() if k != "files"} for r in results],
                "seconds": round(time.time() - t0, 1), "graphics_hle": gfx}
    with open(os.path.join(out, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=2)
    return manifest


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exefs")
    ap.add_argument("--out", required=True)
    ap.add_argument("--per-file", type=int, default=600)
    ap.add_argument("--gfx", action="store_true", help="route the SDK graphics boundary to the host NVN/Vulkan layer")
    a = ap.parse_args(argv)
    print(json.dumps(generate(a.exefs, a.out, a.per_file, a.gfx), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
