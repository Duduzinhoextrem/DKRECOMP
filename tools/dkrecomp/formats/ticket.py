"""Read the public fields of an eShop ticket (.tik).

Only identifiers are returned. The title-key block (offset 0x180 of the ticket data) is deliberately never read
into the result, printed or stored: DKRecomp does not handle keys. Personalized tickets also carry device and
account ids; those are not returned either.

Layout (switchbrew.org/wiki/Ticket): u32 signature type, signature, padding, then the ticket data:
    +0x000 issuer[0x40]  +0x040 title key block[0x100]  +0x140 u8 format version  +0x141 u8 title key type
    +0x142 u16 ticket version  +0x144 u8 license type  +0x145 u8 key generation  +0x146 u16 properties
    +0x150 u64 ticket id  +0x158 u64 device id  +0x160 rights id[0x10]
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

# signature type -> (signature size, padding)
SIG_TYPES = {
    0x010000: (0x200, 0x3C),  # RSA-4096 SHA1
    0x010001: (0x100, 0x3C),  # RSA-2048 SHA1
    0x010002: (0x3C, 0x40),   # ECDSA SHA1
    0x010003: (0x200, 0x3C),  # RSA-4096 SHA256
    0x010004: (0x100, 0x3C),  # RSA-2048 SHA256
    0x010005: (0x3C, 0x40),   # ECDSA SHA256
}


class TicketError(ValueError):
    pass


@dataclass
class TicketInfo:
    signature_type: int
    issuer: str
    title_key_type: str  # "common" or "personalized"
    key_generation: int
    rights_id: str
    title_id: str  # first 8 bytes of the rights id


def parse(raw: bytes) -> TicketInfo:
    if len(raw) < 4:
        raise TicketError("ticket too short")
    sig_type = struct.unpack_from("<I", raw, 0)[0]
    if sig_type not in SIG_TYPES:
        raise TicketError(f"unknown signature type {sig_type:#x}")
    sig, pad = SIG_TYPES[sig_type]
    d = 4 + sig + pad
    if len(raw) < d + 0x170:
        raise TicketError("ticket truncated")
    issuer = raw[d:d + 0x40].split(b"\0", 1)[0].decode("ascii", "replace")
    key_type = raw[d + 0x141]
    key_gen = raw[d + 0x145]
    rights = raw[d + 0x160:d + 0x170]
    return TicketInfo(
        signature_type=sig_type,
        issuer=issuer,
        title_key_type={0: "common", 1: "personalized"}.get(key_type, f"unknown({key_type})"),
        key_generation=key_gen,
        rights_id=rights.hex(),
        title_id=rights[:8].hex(),
    )
