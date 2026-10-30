"""Identify and validate the user's game files without decrypting anything.

    python -m dkrecomp.identify <path> [<path> ...] [--hash] [--out reports/identify]

A path may be an NSP file, a folder holding NSPs, or an extracted ExeFS folder (main, main.npdm, rtld, sdk, ...).
Every statement in the report is read from the files; anything the tool cannot read is reported as unknown.
The report goes to `reports/` (ignored by git). It contains identifiers and hashes only, never keys.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
from dataclasses import asdict
from typing import Any, Dict, List

from .formats import cnmt_xml, npdm, nso, pfs0, ticket
from .titles import match_title


def _fmt_size(n: int) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024
    return str(n)


class Progress:
    def __init__(self, total: int):
        self.total, self.done, self.t0, self.last = total, 0, time.time(), 0.0

    def __call__(self, n: int) -> None:
        self.done += n
        now = time.time()
        if now - self.last > 2 or self.done == self.total:
            self.last = now
            rate = self.done / max(now - self.t0, 1e-6) / (1 << 20)
            print(f"\r  hashing {self.done * 100 // max(self.total, 1):3d}%  {rate:6.0f} MiB/s", end="",
                  file=sys.stderr, flush=True)


def identify_nsp(path: str, do_hash: bool) -> Dict[str, Any]:
    rep: Dict[str, Any] = {"kind": "nsp", "path": os.path.abspath(path), "size": os.path.getsize(path),
                           "problems": [], "notes": []}
    c = pfs0.open_pfs0(path)
    rep["entries"] = [{"name": e.name, "size": e.size} for e in c.entries]

    tiks = c.find(".tik")
    for e in tiks:
        t = ticket.parse(pfs0.read_entry(path, e))
        rep.setdefault("tickets", []).append(asdict(t))
        if not e.name.lower().startswith(t.rights_id):
            rep["problems"].append(f"ticket file name {e.name} does not match its rights id {t.rights_id}")
    if not tiks:
        rep["notes"].append("no ticket in the container")

    metas = c.find(".cnmt.xml")
    if not metas:
        rep["notes"].append("no plaintext .cnmt.xml; title id and version cannot be read without decrypting "
                            "the .cnmt.nca, which DKRecomp does not do")
    for e in metas:
        m = cnmt_xml.parse(pfs0.read_entry(path, e))
        md = asdict(m)
        md["version_release"] = m.version_release
        md["required_system_version_str"] = cnmt_xml.system_version_str(m.required_system_version)
        rep.setdefault("content_meta", []).append(md)
        for t in rep.get("tickets", []):
            if t["title_id"] != m.title_id:
                rep["problems"].append(f"ticket title id {t['title_id']} != content meta id {m.title_id}")
        by_name = {x.name.lower(): x for x in c.entries}
        for ct in m.contents:
            fname = ct.id + (".cnmt.nca" if ct.type == "Meta" else ".nca")
            ent = by_name.get(fname)
            check: Dict[str, Any] = {"type": ct.type, "id": ct.id, "file": fname, "present": ent is not None}
            if ent is None:
                rep["problems"].append(f"{ct.type} content {fname} listed in meta but missing from container")
            else:
                check["size_ok"] = ent.size == ct.size
                if not check["size_ok"]:
                    rep["problems"].append(f"{fname}: size {ent.size} != meta size {ct.size}")
                check["id_is_hash_prefix"] = ct.sha256[:32] == ct.id
                if do_hash:
                    print(f"  {fname} ({_fmt_size(ent.size)})", file=sys.stderr)
                    digest = pfs0.sha256_entry(path, ent, Progress(ent.size)).hex()
                    print(file=sys.stderr)
                    check["sha256"] = digest
                    check["sha256_ok"] = digest == ct.sha256
                    if not check["sha256_ok"]:
                        rep["problems"].append(f"{fname}: SHA-256 mismatch (file damaged or altered)")
            rep.setdefault("content_checks", []).append(check)
        if not do_hash:
            rep["notes"].append("content hashes not verified (run with --hash)")
    known = match_title(rep)
    if known:
        rep["known_release"] = known
    return rep


def _svc_sites(text: bytes) -> Dict[int, int]:
    """Count `SVC #imm` instructions in a text segment (encoding 0xD4000001 | imm16 << 5)."""
    counts: Dict[int, int] = {}
    for (w,) in struct.iter_unpack("<I", text[:len(text) & ~3]):
        if w & 0xFFE0001F == 0xD4000001:
            imm = (w >> 5) & 0xFFFF
            counts[imm] = counts.get(imm, 0) + 1
    return dict(sorted(counts.items()))


def identify_exefs(folder: str) -> Dict[str, Any]:
    rep: Dict[str, Any] = {"kind": "exefs", "path": os.path.abspath(folder), "problems": [], "notes": [],
                           "modules": {}}
    names = sorted(os.listdir(folder))
    meta_path = os.path.join(folder, "main.npdm")
    if os.path.exists(meta_path):
        rep["npdm"] = asdict(npdm.parse(open(meta_path, "rb").read()))
    else:
        rep["problems"].append("main.npdm missing")
    for n in names:
        p = os.path.join(folder, n)
        if n.endswith(".npdm") or not os.path.isfile(p):
            continue
        raw = open(p, "rb").read()
        if raw[:4] != b"NSO0":
            rep["notes"].append(f"{n}: not an NSO, skipped")
            continue
        m = nso.parse(raw)
        text = m.seg("text")
        undefined = [s.name for s in m.symbols if not s.defined and s.name]
        defined = [s for s in m.symbols if s.defined and s.name]
        info = {
            "build_id": m.build_id,
            "segments": [asdict(s) for s in m.segments],
            "bss_size": m.bss_size,
            "segment_hashes_ok": all(s.hash_ok for s in m.segments),
            "mod0": m.mod0_off is not None,
            "needed": m.needed,
            "dynsym_defined": len(defined),
            "dynsym_imported": len(undefined),
            "imports_sample": sorted(undefined)[:50],
            "eh_frame_hdr": list(m.eh_frame_hdr) if m.eh_frame_hdr else None,
            "middleware": nso.middleware(m),
            "svc_sites": _svc_sites(m.image[text.mem_off:text.mem_off + text.size]),
        }
        if not info["segment_hashes_ok"]:
            rep["problems"].append(f"{n}: segment hash mismatch")
        rep["modules"][n] = info
    if "main" not in rep["modules"]:
        rep["problems"].append("main NSO missing")
    return rep


def to_markdown(reports: List[Dict[str, Any]]) -> str:
    out = ["# DKRecomp identification report", "",
           f"Generated {time.strftime('%Y-%m-%d %H:%M:%S')} by `dkrecomp.identify`. Values are read from the "
           "files; nothing here is assumed.", ""]
    for r in reports:
        out += [f"## {os.path.basename(r['path'])} ({r['kind']})", ""]
        if r["kind"] == "nsp":
            out.append(f"- Size: {_fmt_size(r['size'])}")
            for t in r.get("tickets", []):
                out.append(f"- Ticket: rights id `{t['rights_id']}`, title key type {t['title_key_type']}, "
                           f"key generation {t['key_generation']}, issuer `{t['issuer']}`")
            for m in r.get("content_meta", []):
                out.append(f"- Content meta: **{m['type']}** `{m['title_id']}` version {m['version']} "
                           f"(release #{m['version_release']}), required system version "
                           f"{m['required_system_version_str']}")
            if r.get("known_release"):
                out.append(f"- Matches known release: **{r['known_release']}**")
            out += ["", "| Content | File | Size ok | SHA-256 ok |", "|---|---|---|---|"]
            for c in r.get("content_checks", []):
                out.append(f"| {c['type']} | `{c['file']}` | {c.get('size_ok', '-')} | {c.get('sha256_ok', 'not run')} |")
        else:
            if "npdm" in r:
                d = r["npdm"]
                out.append(f"- NPDM: name `{d['name']}`, program id `{d['program_id']}`, 64-bit {d['is_64bit']}, "
                           f"address space {d['address_space_type']}, main thread prio {d['main_thread_priority']} "
                           f"core {d['main_thread_core']} stack {d['main_thread_stack_size']:#x}")
                out.append(f"- Services (client): {', '.join(d['services_client']) or '-'}")
                out.append(f"- SVCs allowed: {', '.join(hex(s) for s in d['svcs'])}")
            for name, m in r["modules"].items():
                segs = ", ".join(f"{s['name']} {s['size']:#x}" for s in m["segments"])
                out.append(f"- `{name}`: build id `{m['build_id'][:32]}`, {segs}, hashes ok {m['segment_hashes_ok']}, "
                           f"imports {m['dynsym_imported']}, exports {m['dynsym_defined']}, SVC sites "
                           f"{sum(m['svc_sites'].values())}")
                for mw in m["middleware"]:
                    out.append(f"  - {mw}")
        for p in r["problems"]:
            out.append(f"- **PROBLEM:** {p}")
        for n in r["notes"]:
            out.append(f"- Note: {n}")
        out.append("")
    return "\n".join(out)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--hash", action="store_true", help="verify the SHA-256 of every content (reads all data)")
    ap.add_argument("--out", default="reports/identify", help="report path without extension")
    a = ap.parse_args(argv)
    reports = []
    def has_nsp(d: str) -> bool:
        return any(f.lower().endswith(".nsp") for _, _, files in os.walk(d) for f in files)

    for p in a.paths:
        if os.path.isdir(p) and has_nsp(p):
            for root, _, files in os.walk(p):
                for f in sorted(files):
                    if f.lower().endswith(".nsp"):
                        print(f"== {f}", file=sys.stderr)
                        reports.append(identify_nsp(os.path.join(root, f), a.hash))
        elif os.path.isdir(p):
            reports.append(identify_exefs(p))
        elif p.lower().endswith(".nsp"):
            reports.append(identify_nsp(p, a.hash))
        else:
            print(f"unsupported input: {p}", file=sys.stderr)
            return 2
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out + ".json", "w", encoding="utf-8") as f:
        json.dump(reports, f, indent=2)
    md = to_markdown(reports)
    with open(a.out + ".md", "w", encoding="utf-8") as f:
        f.write(md)
    print(md)
    return 1 if any(r["problems"] for r in reports) else 0


if __name__ == "__main__":
    sys.exit(main())
