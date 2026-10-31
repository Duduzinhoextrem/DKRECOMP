"""Function discovery helpers of the generator (no game data)."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))

import pytest

from dkrecomp.formats.nso import Nso, NsoError, Segment, Symbol  # noqa: E402
from dkrecomp.gen import branch_target, discover, materialised_code_addresses, unwind_code_addresses  # noqa: E402


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


def unwind_module():
    image = bytearray(0x180)
    image[:0x80] = struct.pack('<I', 0xD65F03C0) * 32
    # The addresses are negative relative to the header, as in modules whose tables follow their text.
    struct.pack_into('<4BiI', image, 0x100, 1, 0x1b, 3, 0x3b, 0x40, 2)
    struct.pack_into('<4i', image, 0x10c, 0x20 - 0x100, 0x140 - 0x100, 0x40 - 0x100, 0x150 - 0x100)
    return Nso('', 0, [Segment('text', 0, 0x80, None)], 0, bytes(image),
               symbols=[Symbol('named', 0, 0x80, 1, 2, 1)], eh_frame_hdr=(0x100, 0x11c))


def test_unwind_table_finds_indirect_only_callbacks():
    module = unwind_module()
    assert unwind_code_addresses(module) == {0x20, 0x40}
    funcs = discover(module, set())
    assert (0x20, 0x80, 'sub_20') in funcs
    assert (0x40, 0x80, 'sub_40') in funcs


def test_truncated_unwind_table_is_reported():
    module = unwind_module()
    module.eh_frame_hdr = (0x100, 0x118)
    with pytest.raises(NsoError, match='truncated'):
        unwind_code_addresses(module)
