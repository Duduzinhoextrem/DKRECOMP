"""Minimal ELF64 little-endian reader: program headers and symbols. Used for DKRecomp's own test programs."""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import List

PT_LOAD = 1
SHT_SYMTAB = 2
STT_FUNC = 2


@dataclass
class Segment:
    vaddr: int
    data: bytes   # file bytes, zero-padded to memsz
    flags: int    # PF_X=1, PF_W=2, PF_R=4


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    type: int


@dataclass
class Elf:
    machine: int
    entry: int
    segments: List[Segment]
    symbols: List[Symbol]

    def functions(self) -> List[Symbol]:
        return sorted((s for s in self.symbols if s.type == STT_FUNC and s.size), key=lambda s: s.value)

    def read(self, addr: int, n: int) -> bytes:
        for s in self.segments:
            if s.vaddr <= addr and addr + n <= s.vaddr + len(s.data):
                return s.data[addr - s.vaddr:addr - s.vaddr + n]
        raise KeyError(f"{addr:#x} not in a loaded segment")


def parse(raw: bytes) -> Elf:
    if raw[:4] != b"\x7fELF" or raw[4] != 2 or raw[5] != 1:
        raise ValueError("not a little-endian ELF64 file")
    (_type, machine, _ver, entry, phoff, shoff, _flags, _ehsize, phentsize, phnum, shentsize, shnum,
     _shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", raw, 16)
    segs = []
    for i in range(phnum):
        p_type, p_flags, p_off, p_vaddr, _pa, p_filesz, p_memsz, _al = struct.unpack_from(
            "<IIQQQQQQ", raw, phoff + i * phentsize)
        if p_type == PT_LOAD and p_memsz:
            segs.append(Segment(p_vaddr, raw[p_off:p_off + p_filesz] + bytes(p_memsz - p_filesz), p_flags))
    sections = [struct.unpack_from("<IIQQQQIIQQ", raw, shoff + i * shentsize) for i in range(shnum)]
    syms = []
    for sh in sections:
        if sh[1] != SHT_SYMTAB:
            continue
        strtab = sections[sh[6]]
        s_off, s_size = strtab[4], strtab[5]
        strs = raw[s_off:s_off + s_size]
        for j in range(sh[5] // 24):
            name, info, _o, _shn, value, size = struct.unpack_from("<IBBHQQ", raw, sh[4] + j * 24)
            end = strs.find(b"\0", name)
            syms.append(Symbol(strs[name:end].decode("utf-8", "replace"), value, size, info & 0xF))
    return Elf(machine, entry, segs, syms)
