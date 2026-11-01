"""Retro shader package parsing, normalization, and rejection of malformed binary inputs.

Fixtures use our own tiny Maxwell shaders; no game files are needed.
"""
import json
from pathlib import Path
import struct
import sys
import zlib

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from dkrecomp.shaderjobs import (Shader, ShaderError, decode_record, export, lzss8,
                                 material_shaders, startup_shaders)
from dkrecomp.testing.shader_probe import maxwell


def package(stage, constants=b''):
    control = bytearray(0x840)
    struct.pack_into('<I', control, 0, 0x98761234)
    struct.pack_into('<I', control, 0x20, len(control))
    header = bytearray(0x100 if stage == 'cs' else 0x30)
    struct.pack_into('<I', header, 0, 0x12345679 if stage == 'cs' else 0x12345678)
    raw = bytes(header) + maxwell(stage)
    struct.pack_into('<II', control, 0x700, len(raw), len(raw) + len(constants))
    if stage == 'cs':
        struct.pack_into('<4I', control, 0x748, 8, 4, 2, 8192)
    if stage == 'ps':
        control[0x718] = 1
    return bytes(control), raw + constants


def form(stages):
    code = b''.join(raw for _, raw in stages)
    controls = b''.join(ctl for ctl, _ in stages)
    compressed = zlib.compress(code + controls)
    record = struct.pack('>IIBI', len(code), len(controls), 0, len(compressed)) + compressed
    # The opaque table deliberately contains a spurious zlib header.
    payload = b'\x78\x00' * 20 + record
    chunk = b'SNVN' + struct.pack('>Q', len(payload)) + bytes(12) + payload
    return b'RFRM' + struct.pack('>Q', len(chunk)) + bytes(8) + b'MTRL' + bytes(8) + chunk


def literals(data):
    return b''.join(b'\x00' + data[i:i + 8] for i in range(0, len(data), 8))


def test_material_program_pairing_constants_and_export(tmp_path):
    data = form([package('vs', bytes(range(16))), package('ps')])
    shaders = material_shaders(data, 'synthetic')
    assert len(shaders) == 2 and shaders[0].program == shaders[1].program
    vertex, code = shaders[0].normalize()
    assert vertex['stage'] == 'vs' and vertex['c1'] == bytes(range(16)).hex()
    assert code[:len(maxwell('vs'))] == maxwell('vs')
    assert shaders[1].normalize()[0]['early_fragment_tests'] is True
    report = export(shaders + shaders[:1], tmp_path)
    jobs = json.loads((tmp_path / 'jobs.json').read_text())
    assert report['unique_stages'] == 2 and report['program_count'] == 1
    assert all(Path(j['code']).is_absolute() and Path(j['code']).is_file() for j in jobs)
    assert report['control_sizes'] == {0x840: 2}


def test_startup_scan_is_not_tied_to_an_executable_offset():
    archive = form([package('vs'), package('ps')])
    image = b'noiseRFRM' + bytes(37) + b'\x01\x00\x00\x00' + literals(archive)
    result = startup_shaders(image, 'module')
    assert [s.normalize()[0]['stage'] for s in result] == ['vs', 'ps']
    assert result[0].source == 'module@0x2e'


def test_control_compile_constants_change_shader_identity():
    ctl, raw = package('vs')
    changed = bytearray(ctl)
    changed[0x7d0] ^= 1
    assert Shader(ctl, raw, '', '').key != Shader(bytes(changed), raw, '', '').key


def test_compute_header_alignment_and_headerless_code():
    ctl, raw = package('cs', bytes(8))
    job, code = Shader(ctl, raw, '', '').normalize()
    assert code == maxwell('cs') + bytes(8)
    assert job['local'] == [8, 4, 2] and job['shared'] == 8192 and job['c1'] == bytes(8).hex()
    headerless = raw[0x100:]
    control = bytearray(ctl)
    struct.pack_into('<II', control, 0x700, len(headerless) - 8, len(headerless))
    assert Shader(bytes(control), headerless, '', '').normalize()[1] == headerless
    damaged = bytearray(raw)
    damaged[0x38] = 1
    with pytest.raises(ShaderError, match='padding'):
        Shader(ctl, bytes(damaged), '', '').normalize()


@pytest.mark.parametrize('kind', ['form', 'chunk', 'control', 'code'])
def test_reject_section_lengths_outside_their_container(kind):
    ctl, raw = package('vs')
    if kind in ('control', 'code'):
        ctl = bytearray(ctl)
        struct.pack_into('<I', ctl, 0x20 if kind == 'control' else 0x704, 0x100000)
    archive = bytearray(form([(ctl, raw)]))
    if kind in ('form', 'chunk'):
        struct.pack_into('>Q', archive, 4 if kind == 'form' else 0x24, len(archive) + 1)
    with pytest.raises(ShaderError):
        material_shaders(bytes(archive), 'bad')


def test_zlib_record_rejects_extra_stream_and_excess_output():
    ctl, raw = package('vs')
    for body, extra in [(raw + ctl + b'excess', b''), (raw + ctl, zlib.compress(b'extra'))]:
        compressed = zlib.compress(body) + extra
        record = struct.pack('>IIBI', len(raw), len(ctl), 0, len(compressed)) + compressed
        assert decode_record(record, 13, len(record), 'bad') is None


def test_lzss_overlap_and_invalid_references():
    # Four literals then a length-six, distance-four reference, with overlapping output.
    assert lzss8(b'\x08ABCD\x30\x04', 10) == b'ABCDABCDAB'
    assert lzss8(b'\x08ABCD\x30\x04', 7) == b'ABCDABC'
    for data in [b'', b'\x00', b'\x80\x00', b'\x80\x00\x00', b'\x80\x00\x01']:
        with pytest.raises(ShaderError):
            lzss8(data, 4)
    with pytest.raises(ShaderError, match='limit'):
        lzss8(b'', (64 << 20) + 1)


def test_reject_vertex_a_without_pairing():
    ctl, raw = package('vs')
    changed = bytearray(raw)
    struct.pack_into('<I', changed, 0x30, 1)
    with pytest.raises(ShaderError, match='VertexA'):
        Shader(ctl, bytes(changed), '', '').normalize()
