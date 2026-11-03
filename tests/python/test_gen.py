"""Module generator: the Ninja file it writes must be valid on Windows paths (no game data needed)."""
import os
import subprocess
import sys
import struct

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))

from dkrecomp import gen  # noqa: E402
from dkrecomp.formats.nso import Nso, Segment, Symbol  # noqa: E402
from dkrecomp.link import LinkedModule  # noqa: E402


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


def test_host_override_executes_for_direct_and_indirect_calls(tmp_path):
    # caller: bl target; ret. The override delegates to the preserved add, then applies host behavior.
    code = struct.pack('<4I', 0x94000002, 0xd65f03c0, 0x91000400, 0xd65f03c0)
    module = Nso('', 0, [Segment('text', 0, len(code), None)], 0, code,
                 symbols=[Symbol('caller', 0, 8, 1, 2, 1), Symbol('canonical_target', 8, 8, 1, 2, 1),
                          Symbol('host_target', 8, 4, 1, 2, 1)])
    linked = LinkedModule('test', module, 0x800000000, len(code), bytearray(code))
    report = gen.lift_module(linked, str(tmp_path), 10, {'host_target': 'test_override'})
    assert len(report['host_overrides']) == 1
    assert report['lifted_words'] == 2
    harness = tmp_path / 'driver.c'
    harness.write_text('''
#include "dkr/rt.h"
#include <stdio.h>
extern void f_800000000(Cpu *);
extern const DkrFnEntry dkr_fn_table_test[];
extern const size_t dkr_fn_count_test;
extern const DkrOriginal dkr_originals_test[];
extern const size_t dkr_original_count_test;
static int called;
void test_override(Cpu *c) {
    called++;
    rt_call_original(c, "test_override");
    c->x[0] = c->x[0] * c->x[1] + 17;
}
int main(void) {
    const DkrModule module = {.name = "test", .originals = dkr_originals_test,
                             .original_count = &dkr_original_count_test};
    rt_set_modules(&module, 1);
    if (!rt_original("test_override") || rt_original("missing")) return 3;
    Cpu cpu = {0};
    cpu.x[0] = 5; cpu.x[1] = 7; cpu.x[19] = 0x12345678; cpu.nzcv = 0x60000000;
    f_800000000(&cpu);
    if (called != 1 || cpu.x[0] != 59 || cpu.x[19] != 0x12345678 || cpu.nzcv != 0x60000000) return 1;
    rt_register_table(dkr_fn_table_test, dkr_fn_count_test, 0x800000000ull, 0x800000010ull);
    cpu.x[0] = 2; cpu.x[1] = 3;
    rt_call(&cpu, 0x800000008ull);
    if (called != 2 || cpu.x[0] != 26 || cpu.x[19] != 0x12345678) return 2;
    puts("PASS: direct and indirect host overrides with original delegation");
    return 0;
}
''')
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
    executable = tmp_path / ('driver.exe' if os.name == 'nt' else 'driver')
    command = [sys.executable, '-m', 'ziglang', 'cc', '-O1', '-mfma', '-ffp-contract=off', '-fno-math-errno',
               '-I' + os.path.join(root, 'runtime', 'include'), str(harness), str(tmp_path / 'f_0000.c'),
               str(tmp_path / 'table.c'), os.path.join(root, 'runtime', 'src', 'core.c'), '-o', str(executable)]
    if os.name == 'nt':
        command[4:4] = ['-target', 'x86_64-windows-gnu']
    built = subprocess.run(command, capture_output=True, text=True, timeout=60)
    assert built.returncode == 0, built.stdout + built.stderr
    run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
    assert run.returncode == 0, run.stdout + run.stderr


def test_movie_overrides_need_the_movie_library_helpers():
    assert gen.movie_overrides({}) == {}
    syms = {name: 0x1000 + 8 * i for i, name in enumerate(gen.MOVIE_SYMBOLS.values())}
    overrides = gen.movie_overrides(syms)
    assert overrides == gen.MOVIE_OVERRIDES
    # Every movie::Decoder member the game imports is replaced, so no SDK DecoderImpl is ever built.
    for member in ("C1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeE", "D1Ev", "9ConfigureEPNS_9MediaDataE",
                   "24SendInputBufferForDecodeEiiilj", "24AcquireOutputBufferIndexEPiPlPjS1_", "24ReleaseOutputBufferIndexEi"):
        assert "_ZN5movie7Decoder" + member in overrides
    assert all(host.startswith("dkr_movie_") for host in overrides.values())
