"""Format readers, tested on synthetic files built here (no game data)."""
import hashlib
import os
import struct
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))

from dkrecomp.formats import cnmt_xml, lz4block, nacp, npdm, nso, pfs0, ticket  # noqa: E402


def make_pfs0(files):
    names = b""
    entries = b""
    data = b""
    for name, blob in files:
        entries += struct.pack("<QQII", len(data), len(blob), len(names), 0)
        names += name.encode() + b"\0"
        data += blob
    names += b"\0" * ((-len(names) - 0x10 - len(entries)) % 0x20)
    return b"PFS0" + struct.pack("<III", len(files), len(names), 0) + entries + names + data


def test_pfs0_roundtrip(tmp_path):
    raw = make_pfs0([("a.tik", b"T" * 10), ("b.nca", b"N" * 1000)])
    p = tmp_path / "x.nsp"
    p.write_bytes(raw)
    c = pfs0.open_pfs0(str(p))
    assert [e.name for e in c.entries] == ["a.tik", "b.nca"]
    assert pfs0.read_entry(str(p), c.entries[0]) == b"T" * 10
    assert pfs0.sha256_entry(str(p), c.entries[1]) == hashlib.sha256(b"N" * 1000).digest()


def test_pfs0_rejects_garbage(tmp_path):
    p = tmp_path / "bad.nsp"
    p.write_bytes(b"NOPE" + bytes(100))
    with pytest.raises(pfs0.Pfs0Error):
        pfs0.open_pfs0(str(p))


def test_pfs0_rejects_truncated(tmp_path):
    raw = make_pfs0([("b.nca", b"N" * 1000)])
    p = tmp_path / "t.nsp"
    p.write_bytes(raw[:-10])
    with pytest.raises(pfs0.Pfs0Error):
        pfs0.open_pfs0(str(p))


def test_ticket_public_fields_only():
    body = bytearray(0x2C0)
    struct.pack_into("<I", body, 0, 0x010004)
    d = 4 + 0x100 + 0x3C
    body[d:d + 6] = b"Root-X"
    body[d + 0x40:d + 0x140] = b"\xAA" * 0x100  # title key block: must never surface
    body[d + 0x141] = 0
    body[d + 0x145] = 4
    rights = bytes.fromhex("0100aaaabbbb00000000000000000004")
    body[d + 0x160:d + 0x170] = rights
    t = ticket.parse(bytes(body))
    assert t.rights_id == rights.hex()
    assert t.title_id == "0100aaaabbbb0000"
    assert t.key_generation == 4 and t.title_key_type == "common"
    assert "aa" * 16 not in repr(t)


def test_cnmt_xml():
    xml = b"""<?xml version="1.0"?><ContentMeta><Type>Patch</Type><Id>0x0100000000001800</Id>
      <Version>131072</Version><Content><Type>Program</Type><Id>00112233445566778899aabbccddeeff</Id>
      <Size>4</Size><Hash>00112233445566778899aabbccddeeff00</Hash><KeyGeneration>4</KeyGeneration></Content>
      <RequiredSystemVersion>269484032</RequiredSystemVersion></ContentMeta>"""
    m = cnmt_xml.parse(xml)
    assert m.type == "Patch" and m.title_id == "0100000000001800"
    assert m.version_release == 2
    assert m.contents[0].type == "Program" and m.contents[0].size == 4
    assert cnmt_xml.system_version_str(269484032).startswith("4.1.0")


def test_lz4_pure_python_matches_reference():
    lz4 = pytest.importorskip("lz4.block")
    src = (b"hello world " * 300) + bytes(range(256)) * 4
    comp = lz4.compress(src, store_size=False)
    assert lz4block.decompress_py(comp, len(src)) == src


def test_lz4_rejects_bad_offset():
    with pytest.raises(lz4block.Lz4Error):
        lz4block.decompress_py(bytes([0x10, 0x41, 0x05, 0x00]), 10)


def make_nso(text: bytes, ro: bytes, data: bytes, bss=0x100):
    hdr = bytearray(0x100)
    hdr[0:4] = b"NSO0"
    off = 0x100
    blobs = []
    mem = 0
    for i, seg in enumerate((text, ro, data)):
        struct.pack_into("<III", hdr, 0x10 + i * 0x10, off, mem, len(seg))
        struct.pack_into("<I", hdr, 0x60 + i * 4, len(seg))
        hdr[0xA0 + i * 0x20:0xC0 + i * 0x20] = hashlib.sha256(seg).digest()
        blobs.append(seg)
        off += len(seg)
        mem += (len(seg) + 0xFFF) & ~0xFFF
    struct.pack_into("<I", hdr, 0x3C, bss)
    struct.pack_into("<I", hdr, 0xC, 0b111000)  # hash checks on, no compression
    hdr[0x40:0x48] = b"BUILDID!"
    return bytes(hdr) + b"".join(blobs)


def test_nso_uncompressed_with_svc_and_middleware():
    text = struct.pack("<II", 0, 0x10) + b"\0" * 8 + b"MOD0" + struct.pack("<iiiii", 0, 0, 0, 0, 0) + b"\0" * 4
    text += struct.pack("<I", 0xD4000001 | (0x26 << 5))  # svc #0x26
    text += b"\0" * ((-len(text)) % 0x1000)
    ro = b"xx SDK MW+Nintendo+NintendoSDK_nnSdk-5_3_0-Release yy".ljust(0x1000, b"\0")
    m = nso.parse(make_nso(text, ro, b"\0" * 0x1000))
    assert all(s.hash_ok for s in m.segments)
    assert m.build_id.startswith(b"BUILDID!".hex())
    assert m.mod0_off == 0x10
    assert nso.middleware(m) == ["SDK MW+Nintendo+NintendoSDK_nnSdk-5_3_0-Release"]


def test_nso_hash_mismatch_detected():
    raw = bytearray(make_nso(b"\0" * 0x1000, b"\0" * 0x1000, b"\0" * 0x1000))
    raw[0x100] = 1
    m = nso.parse(bytes(raw))
    assert m.segments[0].hash_ok is False


def test_npdm_services_and_svcs():
    sac = bytes([0x02]) + b"fsp" + bytes([0x81]) + b"ab"   # client "fsp", server "ab"
    kac = struct.pack("<I", (0b1 << 5) | (0b1 << 7) | (0 << 29) | 0xF)  # SVCs 0 and 2
    kac += struct.pack("<I", 0x7 | (24 << 4) | (59 << 10) | (0 << 16) | (2 << 24))  # thread info
    aci = bytearray(0x40)
    aci[0:4] = b"ACI0"
    struct.pack_into("<Q", aci, 0x10, 0x0100000000001000)
    struct.pack_into("<IIIIII", aci, 0x20, 0x40, 0, 0x40, len(sac), 0x40 + len(sac), len(kac))
    aci += sac + kac
    meta = bytearray(0x80)
    meta[0:4] = b"META"
    meta[0xC] = 0b111
    meta[0xE] = 44
    meta[0x20:0x24] = b"Test"
    struct.pack_into("<I", meta, 0x1C, 0x100000)
    struct.pack_into("<IIII", meta, 0x70, 0x80, len(aci), 0, 0)
    n = npdm.parse(bytes(meta + aci))
    assert n.name == "Test" and n.is_64bit and n.address_space_type == 3
    assert n.services_client == ["fsp"] and n.services_server == ["ab"]
    assert n.svcs == [0, 2]
    assert n.thread_priority_range == (24, 59) and n.core_range == (0, 2)
    assert n.program_id == "0100000000001000"


def test_nacp():
    raw = bytearray(nacp.NACP_SIZE)
    raw[0:5] = b"Hello"
    raw[0x3060:0x3065] = b"1.0.2"
    n = nacp.parse(bytes(raw))
    assert n.titles == {"AmericanEnglish": "Hello"} and n.display_version == "1.0.2"
