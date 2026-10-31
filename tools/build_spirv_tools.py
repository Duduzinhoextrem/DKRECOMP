"""Build the Apache-2.0 Khronos spirv-val validator locally, using pinned sources and Zig C++.

No installer or global dependency is used. Source, licenses, generated headers and output stay in the workspace.
Only SPIRV-Headers is needed; tests, optimizer executables and unrelated test dependencies are not built.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
TOOLS = 'ef96ed763b43b59b33b31b362f09a02b729fa1c9'  # KhronosGroup/SPIRV-Tools v2026.4
HEADERS = '496543121ce6419f23d6fa5d7194ba66c36212d2'  # v2026.4 DEPS


def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def checkout(repository, revision):
    destination = ROOT / 'toolchain' / (repository.lower() + '-' + revision[:12])
    if not (destination / '.git').exists():
        destination.mkdir(parents=True, exist_ok=True)
        run(['git', 'init', '-q', destination])
        run(['git', '-C', destination, 'fetch', '-q', '--depth=1', f'https://github.com/KhronosGroup/{repository}.git', revision])
        run(['git', '-C', destination, 'checkout', '-q', '--detach', 'FETCH_HEAD'])
    actual = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != revision:
        raise SystemExit(f'Unexpected source revision under {destination}: {actual}')
    run(['git', '-C', destination, 'diff', '--quiet', 'HEAD'])
    return destination


def main():
    source, headers = checkout('SPIRV-Tools', TOOLS), checkout('SPIRV-Headers', HEADERS)
    output = ROOT / 'build' / 'spirv-tools'
    binary_dir = Path(sys.executable).parent
    cmake = binary_dir / 'cmake.exe'
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT / 'build/zig-cache'),
               ZIG_LOCAL_CACHE_DIR=str(output / 'zig-cache'))
    run([cmake, '-S', source, '-B', output, '-G', 'Ninja',
         f'-DCMAKE_MAKE_PROGRAM={(binary_dir / "ninja.exe").as_posix()}',
         f'-DCMAKE_TOOLCHAIN_FILE={(ROOT / "tools/toolchain/zig-x86_64-windows.cmake").as_posix()}',
         f'-DPython3_EXECUTABLE={Path(sys.executable).as_posix()}', f'-DSPIRV-Headers_SOURCE_DIR={headers.as_posix()}',
         '-DCMAKE_BUILD_TYPE=Release', '-DSPIRV_SKIP_TESTS=ON', '-DSPIRV_WERROR=OFF',
         '-DSPIRV_TOOLS_BUILD_STATIC=ON'], env=env)
    run([cmake, '--build', output, '--target', 'spirv-val', '-j', '8'], env=env)
    (output / 'source.json').write_text(json.dumps({'SPIRV-Tools': TOOLS, 'SPIRV-Headers': HEADERS}, indent=2) + '\n')
    print(f'Validator built: {output / "tools/spirv-val.exe"}')


if __name__ == '__main__':
    main()
