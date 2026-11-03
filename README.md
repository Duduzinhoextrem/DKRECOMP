# DKRecomp

Português: [README.pt-BR.md](README.pt-BR.md)

DKRecomp turns the Nintendo Switch version of *Donkey Kong Country: Tropical Freeze* into a native Windows x86-64
program. It is a static recompiler: the game's ARM64 code is translated to C once, on your own computer, and compiled
with Clang. No emulator runs while you play.

This repository contains no game files. You need your own copy of the game, and you run the conversion yourself.
DKRecomp does not decrypt anything, does not download game content and never opens your keys. The rules are in
[docs/LEGAL.en.md](docs/LEGAL.en.md).

## Status

The game boots, plays its menus and video cutscenes, and its levels are playable, special stages included. On the
maintainer's machine (RTX 5060 Ti) it runs at 60 fps, with an optional internal resolution of up to 4x. Bugs remain:
see [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) and the measured history in [docs/PROGRESS.md](docs/PROGRESS.md).
Most documents under `docs/` are in Portuguese.

Only the base game, version 1.0.0 (title `0100C1F0051B6000`, v0), is supported.

## Requirements

- Windows 10 or 11, 64-bit, on a CPU with FMA (Intel Haswell, AMD Zen or newer).
- A GPU whose driver supports Vulkan 1.3. Some scenes also use the `VK_EXT_depth_clip_control` and
  `VK_EXT_transform_feedback` extensions.
- About 20 GB of free disk space during setup. The extracted RomFS and the RomFS image take about 7 GB each.
- Python 3.13, Git and the .NET 10 SDK.
- From your own copy of the game: the base game NSP, your `nstool.exe` and your `prod.keys`. `nstool` does the
  decryption on your machine; DKRecomp only passes it the file paths you pick.

## Setup

```bash
git clone <this repository> DKRECOMP
cd DKRECOMP
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements-dev.txt
```

The shader translator is built from a pinned revision of the TotK Recomp project, which carries the MIT-licensed
Ryujinx shader compiler:

```bash
git clone https://github.com/valentderah/totk-pc-recomp refs/totk-pc-recomp
git -C refs/totk-pc-recomp checkout c3b9a4e7f6ec3d1f0a8c84b66b1703b15639a784
```

Then double-click `DKRecomp.bat`. The launcher is in Portuguese:

1. **Preparar a partir do NSP…** asks for your NSP, `nstool.exe` and `prod.keys`. It extracts the code and data,
   generates the C code and compiles it. The first build takes about 10 minutes. The first run also downloads SDL3
   (controllers) and the Vulkan headers from their official sources.
2. **Jogar** starts the game.

The other tabs edit the settings, which are saved to `config.local.toml`. The available keys are described in
[docs/LAUNCHER_E_CONFIGURACOES.md](docs/LAUNCHER_E_CONFIGURACOES.md).

To skip the console window, `.venv/Scripts/python tools/build_launcher_exe.py` builds `DKRecomp.exe`, which opens
the same launcher. To play on another of your own computers without installing Python there,
`.venv/Scripts/python tools/make_portable.py` copies the built game into `DKRecomp-Portatil/`. That folder holds data
from your copy of the game, so keep it to yourself.

## Controls

Xbox, DualShock 4 and DualSense controllers work through SDL3, with XInput as a fallback. On the keyboard: WASD or
the arrow keys to move, Space or K to jump, J or Q to attack and roll, L or E to confirm, Enter or Esc to pause,
Alt+Enter or F11 for full screen.

## How it works

- `tools/dkrecomp/` reads NSP, NSO and NPDM files, decodes ARM64, lifts each function to C and links the modules.
- `runtime/src/` implements the parts of Horizon OS the game calls: threads and synchronization, IPC, and the
  services for input, audio, file system and saves. Video cutscenes are decoded with Windows Media Foundation.
- `runtime/gfx/` records the game's NVN commands and replays them on Vulkan. Maxwell shaders are translated to
  SPIR-V by Ryujinx's shader compiler, running as a separate process (`tools/shader_translator/`).
- `tests/` checks the recompiler against the Unicorn CPU emulator: the same ARM64 code runs in both and must give
  the same result.

More detail in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Development

```bash
.venv/Scripts/python -m pytest
```

Native and Vulkan tests use CMake and are described in [docs/TESTING.md](docs/TESTING.md). Before every push, run
`.venv/Scripts/python tools/check_content.py`, which fails if a game file or anything derived from one is tracked.

## Credits and license

Reference projects: TotK Recomp, LAS Recomp, SwitchRecomp and N64Recomp. Code taken from other projects, with its
license, is listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

DKRecomp's own code is released under the [MIT License](LICENSE). Third-party code keeps its own license. No license
in this repository covers the game or any file taken or generated from it.

DKRecomp is an independent project with no affiliation with Nintendo or Retro Studios. *Donkey Kong Country*,
*Tropical Freeze* and *Nintendo Switch* are trademarks of Nintendo.
