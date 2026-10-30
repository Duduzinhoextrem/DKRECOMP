"""Decoder checks that need no compiler: known encodings, and agreement with Capstone on what is valid."""
import os
import random
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))

from dkrecomp.arm64.decoder import SP, ZR, decode, decode_bit_masks  # noqa: E402
from dkrecomp.testing.insn_cases import FIXED, TEMPLATES, random_word  # noqa: E402

KNOWN = {
    # word: (op, subset of fields)
    0x8B010000: ("addsub_reg", {"sf": 1, "sub": 0, "rd": 0, "rn": 0, "rm": 1}),  # add x0, x0, x1
    0xF100041F: ("addsub_imm", {"sf": 1, "sub": 1, "setflags": 1, "rd": ZR, "imm": 1}),  # cmp x0, #1
    0x910003FD: ("addsub_imm", {"rd": 29, "rn": SP, "imm": 0}),  # mov x29, sp
    0xD65F03C0: ("ret", {"rn": 30}),
    0x94000010: ("bl", {"target": 0x40}),
    0xA9BF7BFD: ("stp", {"rt": 29, "rt2": 30, "rn": SP, "imm": -16, "wb": "pre"}),  # stp x29, x30, [sp, #-16]!
    0xD2800540: ("movz", {"rd": 0, "imm16": 42, "shift": 0}),
    0x9AC20C20: ("sdiv", {"rd": 0, "rn": 1, "rm": 2}),
    0xD4000001: ("svc", {"imm": 0}),
    0x3DC00020: ("vld", {"rt": 0, "rn": 1, "nbytes": 16}),  # ldr q0, [x1]
    0x9E670020: ("fmov_from_gp", {"rd": 0, "rn": 1, "nbytes": 8}),  # fmov d0, x1
    0x1E202000: ("unsupported", {}),  # fcmp s0, s0: FP arithmetic not lifted
}


@pytest.mark.parametrize("word", sorted(KNOWN))
def test_known_encodings(word):
    op, fields = KNOWN[word]
    i = decode(word, 0)
    assert i.op == op
    for k, v in fields.items():
        assert i.f[k] == v, (k, i)


def test_bit_masks():
    # and x0, x0, #0xff : N=1 immr=0 imms=7
    assert decode_bit_masks(1, 7, 0, True, 64)[0] == 0xFF
    # 0x5555... : N=0 immr=0 imms=0b111100
    assert decode_bit_masks(0, 0b111100, 0, True, 64)[0] == 0x5555555555555555
    assert decode_bit_masks(0, 0b111111, 0, True, 64) is None  # reserved


def test_accepted_words_are_valid_for_capstone():
    """Every word the decoder accepts must be a valid instruction for an independent disassembler."""
    capstone = pytest.importorskip("capstone")
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    rng = random.Random(7)
    bad = []
    classes = list(TEMPLATES) + list(FIXED)
    for n in range(20000):
        w = random_word(rng, classes[n % len(classes)])
        i = decode(w, 0x1000)
        if i.op == "unsupported":
            continue
        if next(md.disasm(w.to_bytes(4, "little"), 0x1000), None) is None:
            bad.append(i)
    assert not bad, bad[:10]
