"""Translate synthetic Maxwell shaders; check protocol recovery, SPIR-V structure and stage reflection.

No game code is used. Run after tools/build_shader_translator.py. This checks translation, not GPU execution.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def maxwell(stage):
    # Scheduling word every three instructions. EXIT is predicated PT with condition True.
    exit_word = 0xe30000000007000f
    instructions = [exit_word]
    if stage == 'ps':
        # MOV32I R0..R3 with literal float bits: red, zero, zero, alpha=one.
        instructions = [0x0100000000070000 | (value << 20) | reg
                        for reg, value in enumerate((0x3f800000, 0, 0, 0x3f800000))] + [exit_word]
    words = []
    for i in range(0, len(instructions), 3):
        words += [0] + instructions[i:i + 3]
        words += [0] * (4 - len(words) % 4) if len(words) % 4 else []
    header = bytearray() if stage == 'cs' else bytearray(0x50)
    if header:
        struct.pack_into('<I', header, 0, (1 if stage == 'vs' else 5) << 10)
        if stage == 'ps':
            struct.pack_into('<I', header, 0x48, 0xf)  # RGBA output target 0
    return bytes(header) + struct.pack(f'<{len(words)}Q', *words)


def spirv(path, model):
    raw = path.read_bytes()
    assert len(raw) >= 20 and len(raw) % 4 == 0
    words = struct.unpack(f'<{len(raw) // 4}I', raw)
    assert words[0] == 0x07230203 and words[4] == 0
    position, entries = 5, []
    while position < len(words):
        count, opcode = words[position] >> 16, words[position] & 0xffff
        assert count > 0 and position + count <= len(words)
        if opcode == 15:  # OpEntryPoint
            entries.append(words[position + 1])
        position += count
    assert entries == [model], entries
    return len(raw)


def run(executable, out, validator=None):
    out.mkdir(parents=True, exist_ok=True)
    requests, stages = ['{malformed'], [('vs', 0), ('ps', 4), ('cs', 5)]
    for stage, _ in stages:
        code = out / f'{stage}.maxwell'
        code.write_bytes(maxwell(stage))
        requests.append(json.dumps({'job': {'key': stage, 'stage': stage, 'code': str(code), 'debug': False}, 'out': str(out)}))
    # An invalid key cannot escape the output directory, and the service must accept a later valid job.
    requests.insert(1, json.dumps({'job': {'key': '../escape', 'stage': 'cs', 'code': str(out / 'cs.maxwell')}, 'out': str(out)}))
    requests.insert(2, json.dumps({'job': {'key': 'bad\nline', 'stage': 'cs', 'code': str(out / 'cs.maxwell')}, 'out': str(out)}))
    result = subprocess.run([str(executable), '--serve'], input='\n'.join(requests) + '\n',
                            capture_output=True, text=True, timeout=30)
    replies = result.stdout.splitlines()
    assert result.returncode == 0, result.stderr
    assert len(replies) == 6 and replies[0].startswith('fail ? ') and replies[1].startswith('fail ../escape '), replies
    assert replies[2].startswith('fail ? '), replies
    assert replies[3:] == ['ok vs', 'ok ps', 'ok cs'], (replies, result.stderr)
    sizes = {}
    for stage, model in stages:
        sizes[stage] = spirv(out / f'{stage}.spv', model)
        if validator:
            checked = subprocess.run([str(validator), '--target-env', 'vulkan1.3', str(out / f'{stage}.spv')],
                                     capture_output=True, text=True, timeout=15)
            assert checked.returncode == 0, checked.stdout + checked.stderr
        reflection = json.loads((out / f'{stage}.res.json').read_text())
        assert reflection['stage'] == stage and not reflection['notes'], reflection
        if stage == 'ps':
            assert reflection['frag_out_map'] == 0xf, reflection
    assert not (out.parent / 'escape.spv').exists()
    print('Shader translator: PASS (protocol recovery, key validation, VS/PS/CS SPIR-V, reflection)', sizes,
          'Khronos validation: passed' if validator else 'Khronos validation: not requested')
    return sizes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--translator', type=Path, default=ROOT / 'build/shader_translator/bin/dkr_shader_translator.exe')
    parser.add_argument('--out', type=Path, default=ROOT / 'build/tests/shader_probe')
    parser.add_argument('--validator', type=Path, help='Optional Khronos spirv-val executable')
    args = parser.parse_args()
    run(args.translator.resolve(), args.out.resolve(), args.validator.resolve() if args.validator else None)


if __name__ == '__main__':
    main()
