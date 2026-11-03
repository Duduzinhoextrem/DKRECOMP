"""Build DKRecomp.exe in the project root: a console-less double-click shortcut that opens the launcher.

Usage (from the project root): .venv/Scripts/python tools/build_launcher_exe.py
It uses the same Zig compiler as the rest of the project; the executable is a local build output (not versioned).
"""
from __future__ import annotations

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main() -> int:
    source = os.path.join(ROOT, "tools", "launcher_exe", "dkrecomp.c")
    output = os.path.join(ROOT, "DKRecomp.exe")
    command = [sys.executable, "-m", "ziglang", "cc", "-target", "x86_64-windows-gnu", "-O2", "-municode", "-Wl,--subsystem,windows",
               source, "-o", output, "-luser32"]
    result = subprocess.run(command, cwd=ROOT)
    if result.returncode:
        return result.returncode
    for leftover in ("DKRecomp.pdb", "DKRecomp.lib"):
        path = os.path.join(ROOT, leftover)
        if os.path.isfile(path):
            os.remove(path)
    print(f"criado: {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
