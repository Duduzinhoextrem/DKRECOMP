"""Optional integration with the locally built standalone translator; synthetic code only."""
from pathlib import Path
import sys
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
