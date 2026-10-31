"""Module generator: the Ninja file it writes must be valid on Windows paths (no game data needed)."""
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))

from dkrecomp import gen  # noqa: E402


def test_ninja_file_escapes_drive_colons_and_parses(tmp_path):
    (tmp_path / "main").mkdir()
    for name in ("main/f_0000.c", "main/table.c", "layout.c"):
        (tmp_path / name).write_text("/* empty */\n")
    gen.write_ninja(str(tmp_path), [{"module": "main", "files": ["f_0000.c", "table.c"]}])
    text = (tmp_path / "build.ninja").read_text()
    for line in text.splitlines():
        if line.startswith("build "):
            outputs, _, inputs = line.partition(": ")
            assert ":" not in outputs.replace("$:", "") and ":" not in inputs.replace("$:", ""), line
    ninja = os.path.join(os.path.dirname(sys.executable), "ninja.exe" if os.name == "nt" else "ninja")
    if os.path.exists(ninja):  # ninja must accept the file (dry run, nothing is compiled)
        r = subprocess.run([ninja, "-C", str(tmp_path), "-n"], capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr


def test_write_if_changed_keeps_mtime(tmp_path):
    p = tmp_path / "x.c"
    assert gen.write_if_changed(str(p), "int a;\n")
    assert not gen.write_if_changed(str(p), "int a;\n")
    assert gen.write_if_changed(str(p), "int b;\n")


def test_added_functions_keep_unrelated_chunks_stable(tmp_path):
    funcs = [(i, i + 4, f'f{i}') for i in range(0, 40, 8)]
    original = gen.partition_functions(funcs, str(tmp_path), 2)
    expanded = sorted(funcs + [(4, 8, 'callback')])
    updated = gen.partition_functions(expanded, str(tmp_path), 2)
    assert updated[1:] == original[1:]
    assert updated[0] == [funcs[0], (4, 8, 'callback'), funcs[1]]


def test_existing_c_chunks_migrate_to_stable_layout(tmp_path):
    # Older output has no chunks.json; use its first function definition, ignoring prototypes.
    (tmp_path / 'f_0000.c').write_text('void f_800000010(Cpu *c);\nvoid f_800000000(Cpu *c) {}\n')
    (tmp_path / 'f_0001.c').write_text('void f_800000010(Cpu *c) {}\n')
    funcs = [(i, i + 4, f'f{i}') for i in (0, 4, 8, 16, 24)]
    chunks = gen.partition_functions(funcs, str(tmp_path), 2, 0x800000000)
    assert chunks == [funcs[:3], funcs[3:]]
