"""Parse the plaintext `.cnmt.xml` that some NSP dumps carry next to the encrypted `.cnmt.nca`.

The XML is a copy of the content meta record. It is a convenience, not a root of trust: `identify` checks each
listed content against the SHA-256 of the actual file before relying on it.
"""
from __future__ import annotations

import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import List, Optional


@dataclass
class Content:
    type: str
    id: str
    size: int
    sha256: str
    key_generation: Optional[int]


@dataclass
class ContentMeta:
    type: str
    title_id: str
    version: int
    required_system_version: Optional[int]
    patch_id: Optional[str]
    original_id: Optional[str]
    contents: List[Content] = field(default_factory=list)

    @property
    def version_release(self) -> int:
        """Title versions count releases in steps of 0x10000 (v65536 is the 1st update, v131072 the 2nd)."""
        return self.version >> 16


def _int(node, tag) -> Optional[int]:
    el = node.find(tag)
    return int(el.text, 0) if el is not None and el.text else None


def _txt(node, tag) -> Optional[str]:
    el = node.find(tag)
    return el.text.strip() if el is not None and el.text else None


def parse(raw: bytes) -> ContentMeta:
    # The XML comes from the user's dump; ElementTree does not resolve external entities.
    root = ET.fromstring(raw)
    if root.tag != "ContentMeta":
        raise ValueError(f"unexpected root element {root.tag!r}")
    tid = _txt(root, "Id")
    meta = ContentMeta(
        type=_txt(root, "Type") or "?",
        title_id=f"{int(tid, 16):016x}" if tid else "?",
        version=_int(root, "Version") or 0,
        required_system_version=_int(root, "RequiredSystemVersion"),
        patch_id=_txt(root, "PatchId"),
        original_id=_txt(root, "OriginalId"),
    )
    for c in root.findall("Content"):
        meta.contents.append(Content(
            type=_txt(c, "Type") or "?",
            id=(_txt(c, "Id") or "").lower(),
            size=_int(c, "Size") or 0,
            sha256=(_txt(c, "Hash") or "").lower(),
            key_generation=_int(c, "KeyGeneration"),
        ))
    return meta


def system_version_str(v: Optional[int]) -> str:
    """Horizon version word: major<<26 | minor<<20 | micro<<16 | build. Values above 32 bits are not a single
    version word and are reported raw."""
    if v is None:
        return "?"
    if v >> 32:
        return f"{v} (not a 32-bit version word; meaning not confirmed)"
    return f"{v >> 26}.{(v >> 20) & 0x3F}.{(v >> 16) & 0xF}-{v & 0xFFFF} (raw {v:#x})"
