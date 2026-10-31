"""Module generator: the Ninja file it writes must be valid on Windows paths (no game data needed)."""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))

from dkrecomp import gen  # noqa: E402


def test_ninja_file_escapes_drive_colons_and_parses(tmp_path):
    for name in ("f_0000.c", "fn_table.c"):
        (tmp_path / name).write_text("/* empty */\n")
    gen.write_ninja(str(tmp_path), ["f_0000.c", "fn_table.c"])
    text = (tmp_path / "build.ninja").read_text()
    for line in text.splitlines():
        if line.startswith("build "):
            outputs, _, inputs = line.partition(": ")
            assert ":" not in outputs.replace("$:", "") and ":" not in inputs.replace("$:", ""), line
    ninja = os.path.join(os.path.dirname(sys.executable), "ninja.exe" if os.name == "nt" else "ninja")
    if os.path.exists(ninja):  # ninja must accept the file (dry run, nothing is compiled)
        r = subprocess.run([ninja, "-C", str(tmp_path), "-n"], capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
