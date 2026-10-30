"""NPDM (program metadata) reader: process flags, main thread, services the program may use, allowed SVCs.

switchbrew.org/wiki/NPDM. Only the ACI0 (the program's own access control) is decoded; ACID is signed by Nintendo
and only its presence is reported.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import List, Optional


class NpdmError(ValueError):
    pass


@dataclass
class Npdm:
    name: str
    product_code: str
    is_64bit: bool
    address_space_type: int
    main_thread_priority: int
    main_thread_core: int
    main_thread_stack_size: int
    system_resource_size: int
    version: int
    program_id: Optional[str] = None
    services_client: List[str] = field(default_factory=list)
    services_server: List[str] = field(default_factory=list)
    svcs: List[int] = field(default_factory=list)
    thread_priority_range: Optional[tuple] = None
    core_range: Optional[tuple] = None
    other_kernel_caps: List[str] = field(default_factory=list)

    ADDRESS_SPACES = {0: "32-bit", 1: "64-bit (old, 36-bit)", 2: "32-bit no reserved", 3: "64-bit (39-bit)"}


def _trailing_ones(v: int) -> int:
    n = 0
    while v & 1:
        n += 1
        v >>= 1
    return n


def decode_kernel_caps(words: List[int], out: Npdm) -> None:
    svcs = set()
    for w in words:
        t = _trailing_ones(w)
        if t == 3:  # ThreadInfo
            out.thread_priority_range = ((w >> 4) & 0x3F, (w >> 10) & 0x3F)
            out.core_range = ((w >> 16) & 0xFF, (w >> 24) & 0xFF)
        elif t == 4:  # EnableSystemCalls: 24-bit mask, 3-bit index
            mask, idx = (w >> 5) & 0xFFFFFF, (w >> 29) & 7
            for b in range(24):
                if mask & (1 << b):
                    svcs.add(idx * 24 + b)
        elif w == 0xFFFFFFFF:
            continue  # padding
        else:
            names = {6: "MapRange", 7: "MapIoPage", 10: "MapRegion", 11: "EnableInterrupts", 13: "MiscParams",
                     14: "KernelVersion", 15: "HandleTableSize", 16: "MiscFlags"}
            out.other_kernel_caps.append(f"{names.get(t, f'type{t}')}:{w:#010x}")
    out.svcs = sorted(svcs)


def parse(raw: bytes) -> Npdm:
    if raw[:4] != b"META":
        raise NpdmError(f"not an NPDM (magic {raw[:4]!r})")
    flags = raw[0xC]
    n = Npdm(
        name=raw[0x20:0x30].split(b"\0", 1)[0].decode("ascii", "replace"),
        product_code=raw[0x30:0x40].split(b"\0", 1)[0].decode("ascii", "replace"),
        is_64bit=bool(flags & 1),
        address_space_type=(flags >> 1) & 7,
        main_thread_priority=raw[0xE],
        main_thread_core=raw[0xF],
        system_resource_size=struct.unpack_from("<I", raw, 0x14)[0],
        version=struct.unpack_from("<I", raw, 0x18)[0],
        main_thread_stack_size=struct.unpack_from("<I", raw, 0x1C)[0],
    )
    aci_off, aci_size, _acid_off, _acid_size = struct.unpack_from("<IIII", raw, 0x70)
    aci = raw[aci_off:aci_off + aci_size]
    if aci[:4] != b"ACI0":
        raise NpdmError("ACI0 section missing")
    n.program_id = f"{struct.unpack_from('<Q', aci, 0x10)[0]:016x}"
    _fac_off, _fac_size, sac_off, sac_size, kac_off, kac_size = struct.unpack_from("<IIIIII", aci, 0x20)
    sac = aci[sac_off:sac_off + sac_size]
    i = 0
    while i < len(sac):
        ctl = sac[i]
        ln = (ctl & 7) + 1
        name = sac[i + 1:i + 1 + ln].decode("ascii", "replace")
        (n.services_server if ctl & 0x80 else n.services_client).append(name)
        i += 1 + ln
    kac = aci[kac_off:kac_off + kac_size]
    decode_kernel_caps([struct.unpack_from("<I", kac, j)[0] for j in range(0, len(kac) - 3, 4)], n)
    return n
