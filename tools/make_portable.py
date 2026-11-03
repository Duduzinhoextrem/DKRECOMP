"""Copy an already built game into a folder that runs on another of your own computers, without Python or the NSP.

Usage (from the project root):
    .venv/Scripts/python tools/make_portable.py [--out PASTA] [--no-saves]

The package keeps the project layout the runtime expects (paths are relative to generated/dkctf_v0/dkctf.exe):
    DKRecomp.exe                        opens the game (portable mode: no .venv next to it)
    config.local.toml                   settings (edit by hand; the launcher needs Python)
    generated/dkctf_v0/                 dkctf.exe and the main/sdk/subsdk0 images
    user_files/romfs.bin                game data (~7 GB)
    user_files/saves/                   your saves (unless --no-saves)
    toolchain/sdl3/                     controller support
    build/shader_translator/bin/        translates shaders not yet in the cache (.NET 10 runtime)
    reports/nvn_shader_cache/           shaders already translated on this computer
Everything copied comes from your own copy of the game; keep the package private (docs/LEGAL.md).
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = os.path.join("generated", "dkctf_v0")

README = """DKRecomp - pacote portatil
==========================

Para jogar: de dois cliques em DKRecomp.exe.

Este computador precisa de:
- Windows 10 ou 11 de 64 bits.
- Placa de video com driver atualizado que suporte Vulkan 1.3 (NVIDIA, AMD ou Intel recentes).
- .NET 10 Runtime (https://dotnet.microsoft.com/download/dotnet/10.0, "Runtime" para Windows x64).
  So e usado quando aparece um shader que ainda nao esta no cache, mas sem ele o jogo para nesses pontos.

Configuracoes: edite config.local.toml com o Bloco de Notas (resolucao, render_scale, vsync, fps...).
Saves: user_files\\saves
Se o jogo fechar com erro, o motivo aparece numa janela e fica em generated\\dkctf_v0\\dkctf_boot.log.

As primeiras passagens por cada lugar podem ter pequenas travadas enquanto o cache de pipelines desta
placa de video e montado (reports\\nvn_shader_cache\\pipelines.bin); depois elas somem.

Este pacote contem dados da sua copia do jogo. Use-o apenas nos seus computadores.
"""


def need(path: str) -> str:
    full = os.path.join(ROOT, path)
    if not os.path.exists(full):
        raise SystemExit(f"falta {path}: compile o jogo pelo launcher antes de criar o pacote")
    return full


def copy_file(source: str, destination: str) -> None:
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    if os.path.isfile(destination) and os.path.getsize(destination) == os.path.getsize(source) and \
            os.path.getmtime(destination) >= os.path.getmtime(source):
        return  # already up to date (romfs.bin is 7 GB: re-running the script only copies what changed)
    shutil.copy2(source, destination)


def copy_tree(source: str, destination: str, skip=lambda name: False) -> int:
    count = 0
    for directory, _, files in os.walk(source):
        for name in files:
            if skip(name):
                continue
            path = os.path.join(directory, name)
            copy_file(path, os.path.join(destination, os.path.relpath(path, source)))
            count += 1
    return count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", default=os.path.join(ROOT, "DKRecomp-Portatil"), help="pasta do pacote")
    parser.add_argument("--no-saves", action="store_true", help="nao copiar user_files/saves")
    args = parser.parse_args()
    out = os.path.abspath(args.out)
    if out == ROOT or out.startswith(os.path.join(ROOT, GAME)):
        raise SystemExit("escolha uma pasta de destino separada")

    game = need(GAME)
    images = [n for n in os.listdir(game) if n.endswith(".img")]
    if not images:
        raise SystemExit("faltam as imagens .img do jogo: compile pelo launcher antes de criar o pacote")
    need(os.path.join("user_files", "romfs.bin"))
    translator = need(os.path.join("build", "shader_translator", "bin"))

    steps = [("jogo", lambda: [copy_file(os.path.join(game, n), os.path.join(out, GAME, n)) for n in ["dkctf.exe"] + images])]
    steps.append(("dados do jogo (romfs.bin, ~7 GB)", lambda: copy_file(
        os.path.join(ROOT, "user_files", "romfs.bin"), os.path.join(out, "user_files", "romfs.bin"))))
    steps.append(("tradutor de shaders", lambda: copy_tree(translator, os.path.join(out, "build", "shader_translator", "bin"),
                                                           lambda n: n.endswith(".pdb"))))
    cache = os.path.join(ROOT, "reports", "nvn_shader_cache")
    if os.path.isdir(cache):  # translated shaders only: the Maxwell inputs and the translator log stay here
        steps.append(("cache de shaders", lambda: copy_tree(cache, os.path.join(out, "reports", "nvn_shader_cache"),
                                                            lambda n: not (n.endswith(".dkrspv") or n == "pipelines.bin"))))
    sdl = os.path.join(ROOT, "toolchain", "sdl3")
    if os.path.isdir(sdl):
        steps.append(("suporte a controles (SDL3)", lambda: copy_tree(sdl, os.path.join(out, "toolchain", "sdl3"))))
    saves = os.path.join(ROOT, "user_files", "saves")
    if os.path.isdir(saves) and not args.no_saves:
        steps.append(("saves", lambda: copy_tree(saves, os.path.join(out, "user_files", "saves"))))
    config = os.path.join(ROOT, "config.local.toml")
    if os.path.isfile(config):
        steps.append(("configuracoes", lambda: copy_file(config, os.path.join(out, "config.local.toml"))))

    for description, run in steps:
        print(f"copiando {description}...", flush=True)
        run()

    print("criando DKRecomp.exe...", flush=True)
    result = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "build_launcher_exe.py")], cwd=ROOT)
    if result.returncode:
        return result.returncode
    copy_file(os.path.join(ROOT, "DKRecomp.exe"), os.path.join(out, "DKRecomp.exe"))
    with open(os.path.join(out, "LEIA-ME.txt"), "w", encoding="utf-8", newline="\r\n") as file:
        file.write(README)
    total = sum(os.path.getsize(os.path.join(d, n)) for d, _, files in os.walk(out) for n in files)
    print(f"pronto: {out} ({total / 2**30:.1f} GB). Copie a pasta inteira para o outro computador.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
