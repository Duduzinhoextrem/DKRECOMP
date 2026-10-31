"""NVN driver purity analysis (dkrecomp.nvnmap.Purity) on hand-assembled routines; no game data needed.

A routine served from the original driver must not depend on driver state the host never initialized. The case
that motivated the provenance tracking: nvnTextureBuilderGetStorageSize follows builder->device and reads device
internals, which an argument-only check took for pure."""
import os
import struct
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))

from dkrecomp import nvnmap  # noqa: E402
from dkrecomp.formats.nso import Nso, Segment  # noqa: E402
from dkrecomp.link import LinkedModule  # noqa: E402

pytest.importorskip("capstone")

# Encodings (A64): each routine is a list of 32-bit words.
RET = 0xD65F03C0
STR_W1_X0_18 = 0xB9001801      # str w1, [x0, #0x18]
LDR_X8_X0 = 0xF9400008         # ldr x8, [x0]
LDR_W0_X8_1998 = 0xB9599900    # ldr w0, [x8, #0x1998]
BR_X8 = 0xD61F0100             # br x8
MOV_X0_X8 = 0xAA0803E0         # mov x0, x8
STP_X29_X30 = 0xA9BF7BFD       # stp x29, x30, [sp, #-0x10]!
LDP_X29_X30 = 0xA8C17BFD       # ldp x29, x30, [sp], #0x10


def bl(frm, to):
    return 0x94000000 | (((to - frm) >> 2) & 0x3FFFFFF)


def adrp(rd, frm, page):
    imm = (page - (frm & ~0xFFF)) >> 12
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | rd


def module(routines):
    """Lay routines out in text (0x40 bytes each), then a ro page and a writable data page."""
    text = bytearray(0x1000)
    starts = []
    for n, words in enumerate(routines):
        off = n * 0x40
        starts.append(off)
        for k, w in enumerate(words(off) if callable(words) else words):
            struct.pack_into("<I", text, off + 4 * k, w)
    image = bytes(text) + bytes(0x1000) + bytes(0x1000)
    segs = [Segment("text", 0, 0x1000, None), Segment("ro", 0x1000, 0x1000, None), Segment("data", 0x2000, 0x1000, None)]
    nso = Nso("", 0, segs, 0, image)
    return LinkedModule("sdk", nso, 0x806E00000, len(image), bytearray(image)), starts


def test_purity_tracks_pointer_provenance():
    sdk, at = module([
        [STR_W1_X0_18, RET],                                    # builder setter
        [LDR_X8_X0, LDR_W0_X8_1998, RET],                       # builder->device->field
        lambda o: [adrp(8, o, 0x2000), 0xF9400100, RET],        # ldr x0, [x8] on a writable page
        [BR_X8],                                                # jump-table style dispatch
        lambda o: [STP_X29_X30, bl(o + 4, 0), LDP_X29_X30, RET],  # calls the setter with its own x0
        lambda o: [LDR_X8_X0, MOV_X0_X8, bl(o + 8, 0), RET],    # calls it with a pointer read from memory
    ])
    pure = nvnmap.Purity(sdk)
    verdicts = [pure(a) for a in at]
    assert verdicts[0] == "pure"
    assert verdicts[1].startswith("follows a loaded pointer")
    assert verdicts[2].startswith("reads writable")
    assert verdicts[3].startswith("br")
    assert verdicts[4] == "pure"
    assert verdicts[5].startswith("passes loaded x0")
