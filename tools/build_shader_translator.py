"""Build the managed Maxwell -> SPIR-V translator without NativeAOT or unrelated NuGet packages.

Reads the pinned, unmodified TotK reference. Copies and patches sources only below toolchain/;
all compiler output and .NET/NuGet state stay below build/. Requires the existing .NET 10 SDK.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent
REF = ROOT / 'refs' / 'totk-pc-recomp'
PIN = 'c3b9a4e7f6ec3d1f0a8c84b66b1703b15639a784'
LOCAL = ROOT / 'tools' / 'shader_translator'
WORK = ROOT / 'build' / 'shader_translator'
VENDOR = ROOT / 'toolchain' / 'shader_translator' / ('ryujinx-' + PIN[:12])


def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    revision = subprocess.check_output(['git', '-C', str(REF), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != PIN:
        raise SystemExit(f'The translator requires the pinned TotK reference {PIN}; found {revision}')
    run(['git', '-C', REF, 'diff', '--quiet', 'HEAD', '--', 'tools/xlate', 'tools/vendor/ryujinx'])
    dotnet = shutil.which('dotnet')
    if not dotnet:
        raise SystemExit('The .NET 10 SDK is required; no global installation is performed by this script')
    source = REF / 'tools' / 'vendor' / 'ryujinx'
    identity = hashlib.sha256()
    for path in sorted(source.rglob('*')):
        if not path.is_file() or any(part in ('obj', 'bin') for part in path.relative_to(source).parts):
            continue
        relative = path.relative_to(source)
        target = VENDOR / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        identity.update(relative.as_posix().encode() + b'\0' + hashlib.sha256(path.read_bytes()).digest())
    patches = sorted((REF / 'tools' / 'xlate' / 'patches').glob('*.patch'))
    if len(patches) != 9:
        raise SystemExit('The pinned reference must provide exactly nine reviewed translator patches')
    for patch in patches:
        identity.update(patch.name.encode() + b'\0' + patch.read_bytes())
        for check in (['--check'], []):
            run(['git', 'apply', *check, '--whitespace=nowarn', '--directory', VENDOR.name, patch], cwd=VENDOR.parent)
    for path in [Path(__file__), *sorted(LOCAL.glob('*.cs')), *sorted(LOCAL.glob('*.csproj'))]:
        identity.update(path.name.encode() + b'\0' + path.read_bytes())
    env = dict(os.environ)
    env.update(DOTNET_CLI_HOME=str(WORK / 'dotnet_home'), NUGET_PACKAGES=str(WORK / 'packages'),
               DOTNET_NOLOGO='1', DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1', DOTNET_CLI_TELEMETRY_OPTOUT='1')
    WORK.mkdir(parents=True, exist_ok=True)
    run([dotnet, 'build', LOCAL / 'dkr_shader_translator.csproj', '-c', 'Release',
         f'-p:RyuSrc={VENDOR}', f'-p:BaseIntermediateOutputPath={WORK / "obj"}{os.sep}',
         '-p:InvariantGlobalization=true', '-o', WORK / 'bin'], env=env)
    (WORK / 'bin' / 'dkr_shader_translator.id').write_text(identity.hexdigest() + '\n', encoding='ascii')
    (WORK / 'source.json').write_text(json.dumps({'reference': PIN, 'identity': identity.hexdigest(),
          'backend': 'managed .NET 10', 'nuget_packages': [], 'source': str(VENDOR)}, indent=2) + '\n')
    print(f'Translator built: {WORK / "bin" / "dkr_shader_translator.exe"}')


if __name__ == '__main__':
    main()
