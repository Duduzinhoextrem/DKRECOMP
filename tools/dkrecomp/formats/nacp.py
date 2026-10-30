"""control.nacp reader: title names and the display version (switchbrew.org/wiki/NACP)."""
from __future__ import annotations

from dataclasses import dataclass
from typing import Dict

LANGS = ["AmericanEnglish", "BritishEnglish", "Japanese", "French", "German", "LatinAmericanSpanish", "Spanish",
         "Italian", "Dutch", "CanadianFrench", "Portuguese", "Russian", "Korean", "TraditionalChinese",
         "SimplifiedChinese", "BrazilianPortuguese"]
NACP_SIZE = 0x4000


@dataclass
class Nacp:
    titles: Dict[str, str]
    display_version: str


def parse(raw: bytes) -> Nacp:
    if len(raw) < NACP_SIZE:
        raise ValueError(f"control.nacp is {len(raw)} bytes, expected {NACP_SIZE}")
    titles = {}
    for i, lang in enumerate(LANGS):
        name = raw[i * 0x300:i * 0x300 + 0x200].split(b"\0", 1)[0].decode("utf-8", "replace")
        if name:
            titles[lang] = name
    return Nacp(titles, raw[0x3060:0x3070].split(b"\0", 1)[0].decode("ascii", "replace"))
