"""Assemble our synthetic GPU-test shaders and embed validated SPIR-V in a generated C header."""
import argparse
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assembler', type=Path, required=True)
    parser.add_argument('--validator', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    lines = ['/* Generated from our synthetic SPIR-V assembly; no game data. */', '#include <stdint.h>']
    for name, stage in (('draw', 'vert'), ('draw', 'frag'), ('feedback', 'vert')):
        path = args.out.parent / f'{name}.{stage}.spv'
        subprocess.run([str(args.assembler), '--target-env', 'vulkan1.1', str(args.source / f'{name}.{stage}.spvasm'),
                        '-o', str(path)], check=True)
        subprocess.run([str(args.validator), '--target-env', 'vulkan1.3', str(path)], check=True)
        raw = path.read_bytes()
        words = struct.unpack(f'<{len(raw) // 4}I', raw)
        lines.append(f'static const uint32_t {name}_{stage}[] = {{')
        for i in range(0, len(words), 8):
            lines.append('    ' + ', '.join(f'0x{w:08x}' for w in words[i:i + 8]) + ',')
        lines.append('};')
    args.out.write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
