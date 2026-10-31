"""Function discovery helpers of the generator (no game data)."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))

from dkrecomp.gen import branch_target, materialised_code_addresses  # noqa: E402


def test_branch_targets():
    assert branch_target(0x14000002, 0x100) == 0x108          # b #+8
    assert branch_target(0x97FFFFFF, 0x100) == 0xFC           # bl #-4
    assert branch_target(0x54000040, 0x100) == 0x108          # b.eq #+8
    assert branch_target(0xB4FFFFE0, 0x100) == 0xFC           # cbz x0, #-4
    assert branch_target(0x36080040, 0x100) == 0x108          # tbz w0, #1, #+8
    assert branch_target(0xD65F03C0, 0x100) is None           # ret
    assert branch_target(0x8B010000, 0x100) is None           # add


def test_materialised_addresses():
    t_lo, t_hi = 0x10000, 0x20000
    words = [
        0x10000040,  # 0x10000: adr x0, #+8          -> 0x10008
        0xB0000001,  # 0x10004: adrp x1, #+0x1000     -> page 0x11000
        0xD503201F,  # 0x10008: nop
        0x91010021,  # 0x1000c: add x1, x1, #0x40    -> 0x11040
        0x90000002,  # 0x10010: adrp x2, #0          -> page 0x10000, no matching add: ignored
        0x91000063,  # 0x10014: add x3, x3, #0 (other register)
    ]
    text = b"".join(struct.pack("<I", w) for w in words) + bytes(0x10000 - 4 * len(words))
    assert materialised_code_addresses(text, t_lo, t_hi) == {0x10008, 0x11040}
