"""PFS0 (PartitionFS) reader. An NSP file is a PFS0 container; its header is not encrypted.

Layout (switchbrew.org/wiki/NCA#PFS0):
    0x00 'PFS0'  0x04 u32 file count  0x08 u32 string table size  0x0C u32 reserved
    0x10 entries, 0x18 bytes each: u64 data offset, u64 size, u32 name offset, u32 reserved
    then the string table, then the file data (offsets are relative to the end of the header).
"""
from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass
from typing import BinaryIO, Callable, List, Optional

MAGIC = b"PFS0"
ENTRY_SIZE = 0x18
# Sanity bounds so a corrupt or foreign file fails fast instead of allocating gigabytes.
MAX_FILES = 4096
MAX_STRTAB = 1 << 20


class Pfs0Error(ValueError):
    pass


@dataclass
class Pfs0Entry:
    name: str
    offset: int  # absolute offset in the container file
    size: int


@dataclass
class Pfs0:
    path: str
    header_size: int
    entries: List[Pfs0Entry]

    def find(self, suffix: str) -> List[Pfs0Entry]:
        return [e for e in self.entries if e.name.lower().endswith(suffix.lower())]


def parse_header(f: BinaryIO, path: str = "<stream>", file_size: Optional[int] = None) -> Pfs0:
    head = f.read(0x10)
    if len(head) < 0x10 or head[:4] != MAGIC:
        raise Pfs0Error(f"{path}: not a PFS0 container (magic {head[:4]!r})")
    count, strtab_size, _ = struct.unpack_from("<III", head, 4)
    if count > MAX_FILES or strtab_size > MAX_STRTAB:
        raise Pfs0Error(f"{path}: implausible header (files={count}, strtab={strtab_size})")
    raw_entries = f.read(ENTRY_SIZE * count)
    strtab = f.read(strtab_size)
    if len(raw_entries) != ENTRY_SIZE * count or len(strtab) != strtab_size:
        raise Pfs0Error(f"{path}: truncated header")
    header_size = 0x10 + ENTRY_SIZE * count + strtab_size
    entries = []
    for i in range(count):
        off, size, name_off, _ = struct.unpack_from("<QQII", raw_entries, i * ENTRY_SIZE)
        if name_off >= strtab_size:
            raise Pfs0Error(f"{path}: entry {i} name offset out of range")
        end = strtab.find(b"\0", name_off)
        name = strtab[name_off:end if end >= 0 else None].decode("utf-8", "replace")
        abs_off = header_size + off
        if file_size is not None and abs_off + size > file_size:
            raise Pfs0Error(f"{path}: entry {name!r} extends past end of file")
        entries.append(Pfs0Entry(name, abs_off, size))
    return Pfs0(path, header_size, entries)


def open_pfs0(path: str) -> Pfs0:
    import os
    with open(path, "rb") as f:
        return parse_header(f, path, os.path.getsize(path))


def read_entry(path: str, entry: Pfs0Entry, limit: int = 1 << 24) -> bytes:
    """Read a small entry (ticket, certificate, xml). Refuses entries larger than `limit`."""
    if entry.size > limit:
        raise Pfs0Error(f"{entry.name}: {entry.size} bytes exceeds read limit {limit}")
    with open(path, "rb") as f:
        f.seek(entry.offset)
        return f.read(entry.size)


def sha256_entry(path: str, entry: Pfs0Entry, progress: Optional[Callable[[int], None]] = None,
                 chunk: int = 8 << 20) -> bytes:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        f.seek(entry.offset)
        left = entry.size
        while left:
            buf = f.read(min(chunk, left))
            if not buf:
                raise Pfs0Error(f"{entry.name}: unexpected end of file")
            h.update(buf)
            left -= len(buf)
            if progress:
                progress(len(buf))
    return h.digest()
