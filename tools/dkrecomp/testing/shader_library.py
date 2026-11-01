"""Validate an existing translated shader library against its jobs and reflection.

This verifies SPIR-V validity and records unresolved resource assumptions, not rendering correctness.
The report contains derived game metadata and must remain outside Git.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess

from .shader_probe import spirv

ROOT = Path(__file__).resolve().parents[3]
MODELS = {'vs': 0, 'tcs': 1, 'tes': 2, 'gs': 3, 'ps': 4, 'cs': 5}


def validate(job, directory, validator):
    key, stage = job['key'], job['stage']
    result = {'key': key, 'stage': stage, 'valid': False}
    try:
        path = directory / f'{key}.spv'
        result['bytes'] = spirv(path, MODELS[stage])
        reflected = json.loads((directory / f'{key}.res.json').read_text())
        if reflected['key'] != key or reflected['stage'] != stage:
            raise ValueError('Reflection identity/stage mismatch')
        result['notes'] = reflected['notes']
        checked = subprocess.run([str(validator), '--target-env', 'vulkan1.3', str(path)],
                                 capture_output=True, text=True, timeout=30)
        if checked.returncode:
            raise ValueError(checked.stdout + checked.stderr)
        result['valid'] = True
    except (OSError, ValueError, AssertionError, subprocess.TimeoutExpired) as error:
        result['error'] = str(error)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('jobs', type=Path)
    parser.add_argument('directory', type=Path, help='Existing SPIR-V and .res.json output from the translator')
    parser.add_argument('--validator', type=Path, default=ROOT / 'build/spirv-tools/tools/spirv-val.exe')
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--report', type=Path, help='Default: validation.json alongside jobs.json')
    args = parser.parse_args()
    jobs = json.loads(args.jobs.read_text())
    if not 1 <= args.workers <= 32:
        parser.error('--workers must be between 1 and 32')
    if not args.validator.is_file():
        parser.error('Build spirv-val first or provide --validator')
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        results = list(pool.map(lambda job: validate(job, args.directory, args.validator.resolve()), jobs))
    notes = Counter(n for r in results for n in r.get('notes', []))
    summary = {'total': len(results), 'valid': sum(r['valid'] for r in results),
               'stages': dict(Counter(r['stage'] for r in results)),
               'shaders_with_notes': sum(bool(r.get('notes')) for r in results),
               'notes': dict(notes), 'target': 'vulkan1.3'}
    report = args.report or args.jobs.parent / 'validation.json'
    report.write_text(json.dumps({'summary': summary, 'results': results}, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(summary, indent=2))
    for result in results:
        if not result['valid']:
            print(f"ERROR {result['key']}: {result.get('error', '')}")
    return 0 if results and all(r['valid'] for r in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
