"""Compare device-builder host calls with the user's original SDK in Unicorn; no game files are distributed.

Resolver/guard addresses belong to a particular SDK and must be supplied explicitly. This probe marks only
the local graphics-initialized guard in Unicorn so pure API lookup can run without a Horizon process. It does
not run or claim to validate the original GPU initialization. Output and linked module images stay ignored.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import random
import struct
import subprocess
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X30


def integer(value: str) -> int:
    return int(value, 0)


def compare(args: argparse.Namespace) -> dict:
    image = args.image.read_bytes()
    if args.memory_size < len(image) or args.memory_size & 0xfff or args.base & 0xfff:
        raise ValueError("module memory size/base must be page-aligned and contain the image")
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(args.base, args.memory_size)
    u.mem_write(args.base, image)
    if not args.base <= args.ready_flag < args.base + args.memory_size:
        raise ValueError("graphics guard is outside the module")
    u.mem_write(args.ready_flag, b"\1")
    scratch, stack, stop = 0x3900000000, 0x3900010000, 0x3900020000
    u.mem_map(scratch, 0x10000)
    u.mem_map(stack, 0x10000)
    u.mem_map(stop, 0x1000)

    def call(address: int, x0: int, x1: int) -> int:
        u.reg_write(UC_ARM64_REG_X0, x0)
        u.reg_write(UC_ARM64_REG_X1, x1)
        u.reg_write(UC_ARM64_REG_SP, stack + 0xff00)
        u.reg_write(UC_ARM64_REG_X30, stop)
        u.emu_start(address, stop, count=200_000)
        if u.reg_read(UC_ARM64_REG_PC) != stop:
            raise RuntimeError(f"call to {address:#x} did not return within the instruction limit")
        return u.reg_read(UC_ARM64_REG_X0)

    names = ("nvnDeviceBuilderSetDefaults", "nvnDeviceBuilderSetFlags")
    functions = []
    for name in names:
        u.mem_write(scratch, name.encode("ascii") + b"\0")
        address = call(args.resolver, 0, scratch)
        if not args.base <= address < args.base + args.memory_size:
            raise RuntimeError(f"original resolver did not return a module function for {name}: {address:#x}")
        functions.append(address)

    rng = random.Random(args.seed)
    vectors, expected = bytearray(), bytearray()
    for i in range(args.cases):
        operation, flags = i & 1, rng.getrandbits(64)
        memory = rng.randbytes(64)
        vectors += struct.pack("<QQ", operation, flags) + memory
        u.mem_write(scratch + 0x100, memory)
        call(functions[operation], scratch + 0x110, flags)
        expected += u.mem_read(scratch + 0x100, 64)
    native = subprocess.run([str(args.native.resolve()), "--builder-probe"], input=bytes(vectors), capture_output=True,
                            timeout=30, check=True)
    if len(native.stdout) != len(expected):
        raise RuntimeError(f"native probe returned {len(native.stdout)} bytes, expected {len(expected)}")
    mismatch = [i for i in range(args.cases) if native.stdout[i * 64:(i + 1) * 64] != expected[i * 64:(i + 1) * 64]]
    return {
        "image_sha256": hashlib.sha256(image).hexdigest(),
        "base": hex(args.base), "memory_size": hex(args.memory_size), "resolver": hex(args.resolver),
        "probe_only_ready_guard": hex(args.ready_flag), "functions": dict(zip(names, map(hex, functions))),
        "seed": args.seed, "cases": args.cases, "mismatches": len(mismatch), "first_mismatch": mismatch[:1],
        "scope": "Unicorn original device-builder memory writes versus native host bridge; 64-byte randomized guarded regions",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--base", type=integer, required=True)
    parser.add_argument("--memory-size", type=integer, required=True)
    parser.add_argument("--resolver", type=integer, required=True)
    parser.add_argument("--ready-flag", type=integer, required=True)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--cases", type=int, default=512)
    parser.add_argument("--seed", type=int, default=44)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.cases <= 100_000:
        parser.error("--cases must be between 1 and 100000")
    report = compare(args)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return int(report["mismatches"] != 0)


if __name__ == "__main__":
    raise SystemExit(main())
