"""Known releases of supported titles, loaded from titles/<id>/title.toml.

A release is recognised only by the SHA-256 of its Program content (from the content meta, verified against the
file with --hash). Each title.toml says where each value came from.
"""
from __future__ import annotations

import os
import tomllib
from typing import Any, Dict, Optional

ROOT = os.path.join(os.path.dirname(__file__), "..", "..", "titles")


def load_titles() -> Dict[str, Dict[str, Any]]:
    out = {}
    if not os.path.isdir(ROOT):
        return out
    for d in sorted(os.listdir(ROOT)):
        p = os.path.join(ROOT, d, "title.toml")
        if os.path.exists(p):
            with open(p, "rb") as f:
                out[d] = tomllib.load(f)
    return out


def match_title(nsp_report: Dict[str, Any]) -> Optional[str]:
    programs = {c["id"]: c for m in nsp_report.get("content_meta", []) for c in m["contents"]
                if c["type"] == "Program"}
    for slug, t in load_titles().items():
        for rel in t.get("release", []):
            c = programs.get(rel.get("program_nca_id", ""))
            if c and c["sha256"] == rel.get("program_nca_sha256"):
                verified = any(x.get("sha256_ok") for x in nsp_report.get("content_checks", [])
                               if x["id"] == c["id"])
                tag = "hash verified" if verified else "by content meta only, run --hash to verify"
                return f"{t['name']} {rel['label']} ({slug}, {tag})"
    return None
