"""Prepare Maxwell translation jobs from the user's Retro Studios material archives.

SNVN discovery and startup LZSS8 are adapted from seedds/nxnative shader_split.py
and dmsh_build.py, commit 5474c6f34789a41770868564fbf4e9e32fc57584.
Copyright (c) 2026 seedds. MIT; see runtime/third_party/LICENSE-nxnative.txt.
Only extracted/decompressed inputs are accepted. Outputs contain game data: keep them outside Git.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib

CONTROL_MAGIC = 0x98761234
GRAPHICS_MAGIC = 0x12345678
COMPUTE_MAGIC = 0x12345679
MAX_CODE = 16 << 20
MAX_CONTROL = 1 << 20
MAX_FORM = 64 << 20
STAGES = {1: 'vs', 2: 'tcs', 3: 'tes', 4: 'gs', 5: 'ps'}


class ShaderError(ValueError):
    pass


def u32(data, offset):
    if offset < 0 or offset + 4 > len(data):
        raise ShaderError(f'Truncated word at {offset:#x}')
    return struct.unpack_from('<I', data, offset)[0]


@dataclass(frozen=True)
class Shader:
    control: bytes
    rawcode: bytes
    source: str
    program: str

    @property
    def key(self):
        # Control includes compile-time constants and must participate in identity.
        return hashlib.sha256(self.control + self.rawcode).hexdigest()[:32]

    def normalize(self):
        ctl, raw = self.control, self.rawcode
        own_length = u32(ctl, 0x20)
        if u32(ctl, 0) != CONTROL_MAGIC or not 0x760 <= own_length <= len(ctl) <= MAX_CONTROL:
            raise ShaderError('Invalid control section magic or length')
        c1_offset, length = struct.unpack_from('<II', ctl, 0x700)
        if length != len(raw) or not 0 < length <= MAX_CODE or c1_offset > length:
            raise ShaderError('Control/code lengths or constant segment do not agree')
        magic = u32(raw, 0)
        if magic == GRAPHICS_MAGIC:
            sph = u32(raw, 0x30)
            kind = (sph >> 10) & 15
            stage = STAGES.get(kind)
            if kind == 0 and sph & 31 == 1:
                raise ShaderError('VertexA requires explicit pairing with VertexB')
            if stage is None:
                raise ShaderError(f'Unknown SPH stage {kind}')
            start = 0x30
        else:
            stage = 'cs'
            local = struct.unpack_from('<3I', ctl, 0x748)
            if not all(0 < n <= 1024 for n in local) or local[0] * local[1] * local[2] > 1024:
                raise ShaderError('Invalid compute workgroup size')
            if magic == COMPUTE_MAGIC:
                # NVN compute packages place instructions on the next 0x100 boundary.
                # Removing just the 0x30 header shifts scheduling words and BRX targets.
                if len(raw) < 0x110 or any(raw[0x30:0x100]):
                    raise ShaderError('Unsupported padding in compute code header')
                start = 0x100
            else:
                start = 0
        code = raw[start:]
        if len(code) % 8 or len(code) < (16 if stage == 'cs' else 0x60):
            raise ShaderError('Truncated or unaligned Maxwell code')
        job = {'key': self.key, 'stage': stage, 'c1': raw[c1_offset:].hex(), 'debug': False}
        if stage == 'cs':
            job.update(local=list(local), shared=u32(ctl, 0x754))
        elif stage == 'ps':
            job['early_fragment_tests'] = bool(ctl[0x718])
        return job, code


def decode_record(data, offset, end, label):
    """Try a length-delimited SNVN zlib record. Coincidental header bytes are ignored."""
    if offset < 13 or offset >= end:
        return None
    ncode, nctl, _, compressed = struct.unpack_from('>IIBI', data, offset - 13)
    if not (0 < ncode <= MAX_CODE and 0 < nctl <= MAX_CONTROL and nctl % 0x40 == 0
            and 0 < compressed <= end - offset):
        return None
    expected = ncode + nctl
    decoder = zlib.decompressobj()
    try:
        body = decoder.decompress(data[offset:offset + compressed], expected + 1)
    except zlib.error:
        return None
    if len(body) != expected or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        return None
    shaders, c, k = [], ncode, 0
    program = f'{label}#SNVN@{offset - 13:#x}'
    while c < len(body):
        control = body[c:]
        if u32(control, 0) != CONTROL_MAGIC:
            raise ShaderError(f'{program}: missing control at {c:#x}')
        size = u32(control, 0x20)
        code_size = u32(control, 0x704)
        if not 0x760 <= size <= len(body) - c or not 0 < code_size <= ncode - k:
            raise ShaderError(f'{program}: section extends beyond its record')
        # Later compilers append transform-feedback entries outside the declared section.
        extra_at, extra_size = struct.unpack_from('<II', control, 0x7e8) if size >= 0x7f0 else (0, 0)
        if extra_size:
            if extra_at < size or extra_at + extra_size > len(body) - c:
                raise ShaderError(f'{program}: invalid extra control data')
            size = extra_at + extra_size
        shader = Shader(body[c:c + size], body[k:k + code_size], label, program)
        shader.normalize()
        shaders.append(shader)
        k, c = k + code_size, c + size
    if k != ncode:
        raise ShaderError(f'{program}: code areas do not fill their declared region')
    return compressed, shaders


def material_shaders(data, label):
    """Read big-endian RFRM MTRL, the format used by Tropical Freeze's base SDK 4."""
    if len(data) < 0x20 or data[:4] != b'RFRM' or data[0x14:0x18] != b'MTRL':
        raise ShaderError(f'{label}: expected an RFRM MTRL form')
    form_end = 0x20 + struct.unpack_from('>Q', data, 4)[0]
    if form_end != len(data):
        raise ShaderError(f'{label}: form length does not match its file')
    shaders, p = [], 0x20
    while (p := data.find(b'SNVN', p, form_end)) >= 0:
        if p + 0x18 > form_end:
            raise ShaderError(f'{label}: truncated SNVN chunk')
        end = p + 0x18 + struct.unpack_from('>Q', data, p + 4)[0]
        if end > form_end:
            raise ShaderError(f'{label}: SNVN chunk exceeds the form')
        i = p + 0x18 + 13
        while (i := data.find(b'\x78', i, end)) >= 0:
            record = decode_record(data, i, end, label)
            if record is None:
                i += 1
            else:
                consumed, stages = record
                shaders.extend(stages)
                i += consumed
        p = end
    if not shaders:
        raise ShaderError(f'{label}: no shader programs found')
    return shaders


def lzss8(data, size):
    """Decode exactly size bytes, rejecting invalid/truncated backreferences."""
    if not 0 <= size <= MAX_FORM:
        raise ShaderError('LZSS output exceeds the form limit')
    out, pos, flags, left = bytearray(), 0, 0, 0
    while len(out) < size:
        if not left:
            if pos == len(data):
                raise ShaderError('Truncated LZSS flags')
            flags, pos, left = data[pos], pos + 1, 8
        if flags & 0x80:
            if pos + 2 > len(data):
                raise ShaderError('Truncated LZSS backreference')
            count, distance = (data[pos] >> 4) + 3, ((data[pos] & 15) << 8) | data[pos + 1]
            pos += 2
            if not 0 < distance <= len(out):
                raise ShaderError('LZSS backreference precedes output')
            for _ in range(min(count, size - len(out))):
                out.append(out[-distance])
        else:
            if pos == len(data):
                raise ShaderError('Truncated LZSS literal')
            out.append(data[pos])
            pos += 1
        flags, left = flags << 1 & 255, left - 1
    return bytes(out)


def startup_shaders(image, label):
    shaders = []
    for match in re.finditer(b'RFRM', image):
        p = match.start()
        if p < 5 or image[p - 5:p - 1] != b'\x01\x00\x00\x00':
            continue
        compressed = memoryview(image)[p - 1:]
        try:
            head = lzss8(compressed, 0x20)
        except ShaderError:
            continue
        if head[:4] != b'RFRM' or head[0x14:0x18] != b'MTRL':
            continue
        size = 0x20 + struct.unpack_from('>Q', head, 4)[0]
        form = lzss8(compressed, size)
        shaders.extend(material_shaders(form, f'{label}@{p - 5:#x}'))
    if not shaders:
        raise ShaderError(f'{label}: no compressed startup material found')
    return shaders


def export(shaders, out):
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    jobs, programs, controls = {}, {}, Counter()
    for shader in shaders:
        job, code = shader.normalize()
        key = shader.key
        path = out / f'{key}.maxwell'
        job['code'] = str(path)
        if key not in jobs:
            path.write_bytes(code)
            (out / f'{key}.control').write_bytes(shader.control)
            jobs[key] = job
            controls[len(shader.control)] += 1
        programs.setdefault(shader.program, []).append({'key': key, 'stage': job['stage']})
    (out / 'jobs.json').write_text(json.dumps(list(jobs.values()), indent=2) + '\n', encoding='utf-8')
    summary = {'schema': 1, 'programs': programs, 'unique_stages': len(jobs),
               'stage_counts': dict(Counter(j['stage'] for j in jobs.values())),
               'control_sizes': dict(controls),
               'resource_metadata': 'Not inferred: validate texture/image types against runtime bindings before use'}
    (out / 'inventory.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    return {k: v for k, v in summary.items() if k != 'programs'} | {'program_count': len(programs)}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--main', type=Path, help='Extracted main NSO, for the compressed startup material')
    parser.add_argument('--materials', type=Path, help='MaterialArchive.arc from the extracted RomFS')
    parser.add_argument('--out', type=Path, default=Path('reports/shader_jobs'), help='Ignored local output directory')
    args = parser.parse_args()
    if not (args.main or args.materials):
        parser.error('Provide --main and/or --materials')
    shaders = []
    if args.main:
        from .formats.nso import parse
        image = parse(args.main.read_bytes()).image
        shaders += startup_shaders(image, str(args.main))
    if args.materials:
        shaders += material_shaders(args.materials.read_bytes(), str(args.materials))
    print(json.dumps(export(shaders, args.out), indent=2))


if __name__ == '__main__':
    main()
