"""NSO0 executable reader (switchbrew.org/wiki/NSO).

Header (0x100 bytes):
    0x00 'NSO0'  0x04 version  0x0C flags: bit0-2 text/ro/data LZ4-compressed, bit3-5 check segment hash
    0x10 text {file off, mem off, size}  0x1C module name offset
    0x20 ro   {file off, mem off, size}  0x2C module name size
    0x30 data {file off, mem off, size}  0x3C bss size
    0x40 module id (build id) [0x20]
    0x60 text/ro/data compressed sizes   0x88 api info, 0x90 dynstr, 0x98 dynsym (ro-relative offset, size)
    0xA0/0xC0/0xE0 SHA-256 of the decompressed text/ro/data
"""
from __future__ import annotations

import hashlib
import re
import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

from . import lz4block

DT_NULL, DT_NEEDED, DT_STRTAB, DT_SYMTAB, DT_STRSZ, DT_SYMENT = 0, 1, 5, 6, 10, 11
DT_RELA, DT_RELASZ, DT_JMPREL, DT_PLTRELSZ, DT_RELR = 7, 8, 23, 2, 0x24


class NsoError(ValueError):
    pass


@dataclass
class Segment:
    name: str
    mem_off: int
    size: int
    hash_ok: Optional[bool]


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    bind: int
    type: int
    shndx: int

    @property
    def defined(self) -> bool:
        return self.shndx != 0


@dataclass
class Nso:
    build_id: str
    flags: int
    segments: List[Segment]
    bss_size: int
    image: bytes  # text|ro|data laid out at their memory offsets, bss not included
    mod0_off: Optional[int] = None
    dynamic: Dict[int, List[int]] = field(default_factory=dict)
    needed: List[str] = field(default_factory=list)
    symbols: List[Symbol] = field(default_factory=list)
    eh_frame_hdr: Optional[Tuple[int, int]] = None

    def seg(self, name: str) -> Segment:
        return next(s for s in self.segments if s.name == name)

    def u32(self, off: int) -> int:
        return struct.unpack_from("<I", self.image, off)[0]

    def strings(self, min_len: int = 6, segment: str = "ro") -> List[str]:
        s = self.seg(segment)
        blob = self.image[s.mem_off:s.mem_off + s.size]
        return [m.group().decode("ascii") for m in re.finditer(rb"[\x20-\x7e]{%d,}" % min_len, blob)]


def parse(raw: bytes, check_hashes: bool = True) -> Nso:
    if raw[:4] != b"NSO0":
        raise NsoError(f"not an NSO (magic {raw[:4]!r})")
    if len(raw) < 0x100:
        raise NsoError("truncated header")
    flags = struct.unpack_from("<I", raw, 0xC)[0]
    names = ("text", "ro", "data")
    segs, blobs = [], []
    for i, name in enumerate(names):
        file_off, mem_off, size = struct.unpack_from("<III", raw, 0x10 + i * 0x10)
        comp = struct.unpack_from("<I", raw, 0x60 + i * 4)[0]
        if file_off + comp > len(raw):
            raise NsoError(f"{name}: segment past end of file")
        data = raw[file_off:file_off + comp]
        if flags & (1 << i):
            data = lz4block.decompress(data, size)
        elif len(data) != size:
            raise NsoError(f"{name}: uncompressed size mismatch")
        ok = None
        if check_hashes:
            want = raw[0xA0 + i * 0x20:0xC0 + i * 0x20]
            ok = hashlib.sha256(data).digest() == want
        segs.append(Segment(name, mem_off, size, ok))
        blobs.append((mem_off, data))
    bss = struct.unpack_from("<I", raw, 0x3C)[0]
    end = max(off + len(d) for off, d in blobs)
    img = bytearray(end)
    for off, d in blobs:
        img[off:off + len(d)] = d
    nso = Nso(build_id=raw[0x40:0x60].hex(), flags=flags, segments=segs, bss_size=bss, image=bytes(img))
    _parse_mod0(nso)
    return nso


def _parse_mod0(nso: Nso) -> None:
    img = nso.image
    if len(img) < 8:
        return
    mod0 = struct.unpack_from("<I", img, 4)[0]
    if mod0 + 0x1C > len(img) or img[mod0:mod0 + 4] != b"MOD0":
        return
    nso.mod0_off = mod0
    dyn_rel, _bss_s, _bss_e, eh_s, eh_e = struct.unpack_from("<iiiii", img, mod0 + 4)
    if eh_e > eh_s:
        nso.eh_frame_hdr = (mod0 + eh_s, mod0 + eh_e)
    dyn = mod0 + dyn_rel
    while dyn + 16 <= len(img):
        tag, val = struct.unpack_from("<qQ", img, dyn)
        dyn += 16
        if tag == DT_NULL:
            break
        nso.dynamic.setdefault(tag, []).append(val)
    strtab = nso.dynamic.get(DT_STRTAB, [None])[0]
    strsz = nso.dynamic.get(DT_STRSZ, [0])[0]
    symtab = nso.dynamic.get(DT_SYMTAB, [None])[0]
    if strtab is None or symtab is None or strtab + strsz > len(img):
        return

    def cstr(off: int) -> str:
        end = img.find(b"\0", strtab + off)
        return img[strtab + off:end].decode("utf-8", "replace")

    nso.needed = [cstr(v) for v in nso.dynamic.get(DT_NEEDED, [])]
    # Linker-generated NSOs put .dynstr right after .dynsym, so the symbol count is the gap / 24.
    count = (strtab - symtab) // 24 if strtab > symtab else 0
    for i in range(count):
        name_off, info, _other, shndx, value, size = struct.unpack_from("<IBBHQQ", img, symtab + i * 24)
        nso.symbols.append(Symbol(cstr(name_off) if name_off else "", value, size, info >> 4, info & 0xF, shndx))


SDK_MARKER = re.compile(r"SDK MW\+[\w+.\-]+")


def middleware(nso: Nso) -> List[str]:
    """Middleware strings Nintendo's SDK embeds (`SDK MW+Vendor+Component-version`)."""
    s = nso.seg("ro")
    blob = nso.image[s.mem_off:s.mem_off + s.size].decode("latin-1")
    return sorted(set(SDK_MARKER.findall(blob)))
