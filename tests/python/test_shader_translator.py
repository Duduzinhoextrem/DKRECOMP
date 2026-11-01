"""Optional integration with the locally built standalone translator; synthetic code only."""
from pathlib import Path
import sys
import subprocess
import json
import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from dkrecomp.testing.shader_probe import run


def test_shader_translator_protocol_and_spirv(tmp_path):
    executable = ROOT / 'build/shader_translator/bin/dkr_shader_translator.exe'
    if not executable.exists():
        pytest.skip('Build the optional shader translator first')
    validator = ROOT / 'build/spirv-tools/tools/spirv-val.exe'
    sizes = run(executable, tmp_path, validator if validator.exists() else None)
    assert set(sizes) == {'vs', 'ps', 'cs'}


def test_native_persistent_compiler_and_disk_cache(tmp_path):
    executable = ROOT / 'build/shader_translator/bin/dkr_shader_translator.exe'
    native = ROOT / 'build/zig/dkr_nvn_test.exe'
    if not executable.exists() or not native.exists():
        pytest.skip('Build the optional translator and native NVN test first')
    result = subprocess.run([str(native), '--compiler-probe', str(tmp_path)], capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stdout + result.stderr
    assert json.loads(result.stdout) == {'translations': 6, 'failures': 0}
    assert len(list(tmp_path.glob('*.dkrspv'))) == 3 and len(list(tmp_path.glob('*.maxwell'))) == 3
    assert not list(tmp_path.glob('*.tmp'))
    # A new host process must consume the same caches without starting a compiler or rewriting its inputs.
    stamps = {p.name: p.stat().st_mtime_ns for p in tmp_path.iterdir()}
    result = subprocess.run([str(native), '--compiler-probe', str(tmp_path)], capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stdout + result.stderr
    assert {p.name: p.stat().st_mtime_ns for p in tmp_path.iterdir()} == stamps


def test_vertex_shader_declares_unlinked_outputs_for_interface_matching(tmp_path):
    executable = ROOT / 'build/shader_translator/bin/dkr_shader_translator.exe'
    validator = ROOT / 'build/spirv-tools/tools/spirv-val.exe'
    if not executable.exists():
        pytest.skip('Build the optional shader translator first')
    from dkrecomp.testing.shader_probe import maxwell
    import struct

    vs_code = tmp_path / 'vs.maxwell'
    vs_code.write_bytes(maxwell('vs'))
    req = json.dumps({'job': {'key': 'vs', 'stage': 'vs', 'code': str(vs_code), 'debug': False}, 'out': str(tmp_path)})
    res = subprocess.run([str(executable), '--serve'], input=req + '\n', capture_output=True, text=True, timeout=15)
    assert res.returncode == 0 and res.stdout.strip() == 'ok vs'

    if validator.exists():
        val = subprocess.run([str(validator), '--target-env', 'vulkan1.3', str(tmp_path / 'vs.spv')],
                             capture_output=True, text=True, timeout=15)
        assert val.returncode == 0, val.stdout + val.stderr

    raw = (tmp_path / 'vs.spv').read_bytes()
    words = struct.unpack(f'<{len(raw)//4}I', raw)
    var_sc, var_loc = {}, {}
    i = 5
    while i < len(words):
        op = words[i] & 0xffff
        len_w = words[i] >> 16
        if op == 59:
            var_sc[words[i+2]] = words[i+3]
        elif op == 71 and words[i+2] == 30:  # OpDecorate Location
            var_loc[words[i+1]] = words[i+3]
        i += len_w
    outputs = {var_loc[v] for v in var_sc if var_sc[v] == 3 and v in var_loc}
    # All 16 standard user attribute locations (0..15) must be declared to satisfy VUID-RuntimeSpirv-OpEntryPoint-08743.
    assert set(range(16)).issubset(outputs), f'Expected locations 0..15, got {sorted(outputs)}'

