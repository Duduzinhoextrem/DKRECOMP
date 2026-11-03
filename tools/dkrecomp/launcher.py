"""DKRecomp launcher: prepares the game from the user's own NSP, edits the settings and starts the game.

    .venv/Scripts/pythonw -m dkrecomp.launcher        (DKRecomp.bat at the repository root does this)

Settings go to config.local.toml at the repository root (ignored by Git), read by the runtime at start
(runtime/src/settings.c). Preparing from an NSP runs the user's own nstool with the user's own keys, as the files in
user_files/ were produced before; DKRecomp itself decrypts nothing and never reads the keys.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tomllib
import math
import uuid
from dataclasses import dataclass
from typing import Callable, Dict, List, Optional, Tuple

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CONFIG = os.path.abspath(os.environ.get("DKR_CONFIG") or os.path.join(ROOT, "config.local.toml"))
USER = os.path.join(ROOT, "user_files")
GENERATED = os.path.join(ROOT, "generated", "dkctf_v0")
EXE = os.path.join(GENERATED, "dkctf.exe")
PYTHON = sys.executable.replace("pythonw.exe", "python.exe")

# ---------------------------------------------------------------- settings (subset of TOML, see runtime/src/settings.c)
DEFAULTS: Dict[str, str] = {
    "display_mode": "windowed", "window_width": "0", "window_height": "0", "render_scale": "1.0",
    "vsync": "mailbox", "fps_limit": "60", "present_filter": "linear", "keep_aspect": "true",
    "show_stats": "false", "mute": "false", "movie_smoothing": "true",
    "input_backend": "auto", "controller_index": "0", "second_controller_index": "1", "stick_deadzone": "15", "swap_abxy": "false",
    "input_background": "false",
}
RENDER_PRESETS: List[Tuple[str, str]] = [  # label, render_scale (the game renders 1920x1080)
    ("720p (mais leve)", "0.667"), ("1080p (original)", "1.0"), ("1440p", "1.333"), ("1800p", "1.667"),
    ("4K / 2160p", "2.0"), ("5K / 2880p", "2.667"), ("8K / 4320p (muito pesado)", "4.0"),
]
WINDOW_PRESETS: List[Tuple[str, str, str]] = [
    ("Automático (75% do monitor)", "0", "0"), ("1280 x 720", "1280", "720"), ("1600 x 900", "1600", "900"),
    ("1920 x 1080", "1920", "1080"), ("2560 x 1440", "2560", "1440"), ("3840 x 2160", "3840", "2160"),
]
FPS_PRESETS: List[Tuple[str, str]] = [
    ("30", "30"), ("60 (original)", "60"), ("90", "90"), ("120", "120"), ("144", "144"), ("165", "165"),
    ("240", "240"), ("Sem limite", "0"),
]
VSYNC_PRESETS: List[Tuple[str, str]] = [
    ("Mailbox (recomendado: sem tearing, baixa latência)", "mailbox"), ("Ligado (FIFO, limita à taxa do monitor)", "on"),
    ("Desligado (pode ter tearing)", "off"),
]
KEYS = [("Analógico esquerdo", "W A S D / setas"), ("A (confirmar)", "L ou E"), ("B (pular)", "Espaço ou K"),
        ("X", "I"), ("Y (ataque / rolar)", "J ou Q"), ("L / R", "U ou Ctrl  /  O ou V"), ("ZL / ZR", "Shift  /  C"),
        ("+ (pausa)", "Enter ou Esc"), ("−", "Backspace ou Tab"), ("Tela cheia", "Alt+Enter ou F11"),
        ("Controle", "Xbox, DualShock 4 e DualSense: SDL3 / XInput")]

STRING_OPTIONS = {"display_mode": ("windowed", "borderless"), "vsync": ("mailbox", "on", "off"),
                  "present_filter": ("linear", "nearest"), "input_backend": ("auto", "xinput", "keyboard")}
BOOL_KEYS = {"movie_smoothing", "keep_aspect", "show_stats", "mute", "swap_abxy", "input_background"}


def valid_value(key: str, value: str) -> bool:
    if key in STRING_OPTIONS:
        return value in STRING_OPTIONS[key]
    if key in BOOL_KEYS:
        return value in ("true", "false")
    try:
        if key == "render_scale":
            return math.isfinite(float(value)) and 0.5 <= float(value) <= 4.0
        n = int(value)
        if key in ("window_width", "window_height"):
            return n == 0 or 320 <= n <= 16384
        return 0 <= n <= {"fps_limit": 1000, "controller_index": 3, "second_controller_index": 4, "stick_deadzone": 50}[key]
    except (ValueError, KeyError):
        return False


def read_settings(path: str = CONFIG) -> Dict[str, str]:
    values = dict(DEFAULTS)
    try:
        with open(path, "rb") as f:
            data = tomllib.load(f)
        groups = [data] + [v for v in data.values() if isinstance(v, dict)]
        for group in groups:
            for key, value in group.items():
                text = str(value).lower() if isinstance(value, bool) else str(value)
                if key in DEFAULTS and valid_value(key, text):
                    values[key] = text
    except (OSError, tomllib.TOMLDecodeError):
        pass
    return values


def write_settings(values: Dict[str, str], path: str = CONFIG) -> None:
    for key in DEFAULTS:
        if not valid_value(key, values.get(key, DEFAULTS[key])):
            raise ValueError(f"Valor inválido para {key}")
    def q(k: str) -> str:
        v = values.get(k, DEFAULTS[k])
        return f'"{v}"' if k in STRING_OPTIONS else v
    text = "\n".join([
        "# DKRecomp settings, written by the launcher (DKRecomp.bat). Read by the game at start.",
        "[display]", f"display_mode = {q('display_mode')}", f"window_width = {q('window_width')}",
        f"window_height = {q('window_height')}", f"keep_aspect = {q('keep_aspect')}", f"present_filter = {q('present_filter')}",
        "", "[graphics]", f"render_scale = {q('render_scale')}", f"movie_smoothing = {q('movie_smoothing')}",
        "", "[performance]", f"fps_limit = {q('fps_limit')}", f"vsync = {q('vsync')}", f"show_stats = {q('show_stats')}",
        "", "[audio]", f"mute = {q('mute')}", "", "[input]", f"input_backend = {q('input_backend')}",
        f"controller_index = {q('controller_index')}", f"second_controller_index = {q('second_controller_index')}", f"stick_deadzone = {q('stick_deadzone')}",
        f"swap_abxy = {q('swap_abxy')}", f"input_background = {q('input_background')}", ""])
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


# ---------------------------------------------------------------- game files
@dataclass
class Status:
    exefs: bool
    romfs: bool
    built: bool
    stale: bool
    images: bool = True
    translator: bool = True

    @property
    def ready(self) -> bool:
        return self.exefs and self.romfs and self.built and self.images and self.translator


def status() -> Status:
    exefs = all(os.path.isfile(os.path.join(USER, "exefs", n)) for n in ("main", "main.npdm", "sdk", "subsdk0"))
    romfs = os.path.isfile(os.path.join(USER, "romfs.bin")) and os.path.getsize(os.path.join(USER, "romfs.bin")) > 1 << 30
    built = os.path.isfile(EXE)
    stale = False
    if built:
        exe_time = os.path.getmtime(EXE)
        for folder in (os.path.join(ROOT, "runtime"),):
            for base, _, files in os.walk(folder):
                if any(f.endswith((".c", ".h", ".py")) and os.path.getmtime(os.path.join(base, f)) > exe_time for f in files):
                    stale = True
                    break
        for relative in ("gen.py", "link.py", "arm64/decoder.py", "recomp/lifter.py"):
            if os.path.getmtime(os.path.join(ROOT, "tools", "dkrecomp", relative)) > exe_time:
                stale = True
    images = all(os.path.isfile(os.path.join(GENERATED, n + ".img")) for n in ("main", "sdk", "subsdk0"))
    translator_exe = os.path.join(ROOT, "build", "shader_translator", "bin", "dkr_shader_translator.exe")
    identity = os.path.splitext(translator_exe)[0] + ".id"
    translator = os.path.isfile(translator_exe) and os.path.isfile(identity)
    if translator and any(os.path.getmtime(os.path.join(ROOT, "tools", "shader_translator", n)) > os.path.getmtime(identity)
                          for n in ("Program.cs", "dkr_shader_translator.csproj")):
        stale = True
    return Status(exefs, romfs, built, stale, images, translator)


def default_tool(name: str) -> str:
    p = os.path.join(ROOT, "SwitchExtract", name)
    return p if os.path.exists(p) else ""


# description, then a command line or a Python callable taking the log function, then an output that must exist after it
Step = Tuple[str, object, Optional[str]]


def copy_entries(nsp: str, names: List[str], out_dir: str) -> Callable[[Callable[[str], None]], None]:
    """Copy entries out of the NSP (a plain PFS0 container: no decryption) with DKRecomp's own reader."""
    def run(log: Callable[[str], None]) -> None:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        from dkrecomp.formats import pfs0
        container = pfs0.open_pfs0(nsp)
        os.makedirs(out_dir, exist_ok=True)
        for name in names:
            if not name or any(c in name for c in ("/", "\\", ":")) or name in (".", ".."):
                raise ValueError("Nome de entrada inválido no NSP")
            entry = next((e for e in container.entries if e.name.lower() == name.lower()), None)
            if entry is None:
                raise RuntimeError(f"{name} não está no NSP")
            target = os.path.join(out_dir, name)
            with open(nsp, "rb") as src, open(target + ".partial", "wb") as dst:
                src.seek(entry.offset)
                left, done = entry.size, 0
                while left:
                    chunk = src.read(min(left, 8 << 20))
                    if not chunk:
                        raise RuntimeError(f"{name}: o NSP terminou antes do fim da entrada")
                    dst.write(chunk); left -= len(chunk); done += len(chunk)
                    if entry.size > 1 << 30 and done % (1 << 30) < (8 << 20):
                        log(f"  {name}: {done >> 20} MiB de {entry.size >> 20} MiB\n")
            os.replace(target + ".partial", target)
            log(f"  {name}: {entry.size} bytes\n")
    return run


def nsp_plan(nsp: str) -> Tuple[str, str, str, str]:
    """(program NCA entry, ticket entry, release label, title id) read from the NSP's plaintext metadata."""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    from dkrecomp import identify
    rep = identify.identify_nsp(nsp, do_hash=False)
    programs = [c for m in rep.get("content_meta", []) for c in m["contents"] if c["type"] == "Program"]
    if not programs:
        raise RuntimeError("o NSP não tem metadados legíveis (.cnmt.xml) com um conteúdo Program")
    tickets = [e["name"] for e in rep.get("entries", []) if e["name"].lower().endswith(".tik")]
    if not tickets:
        raise RuntimeError("o NSP não tem ticket (.tik)")
    title = rep["content_meta"][0]["title_id"]
    if title.lower() != "0100c1f0051b6000" or rep["content_meta"][0].get("version") != 0:
        raise ValueError("Somente o jogo base 0100C1F0051B6000, versão v0, é suportado.")
    return programs[0]["id"] + ".nca", tickets[0], rep.get("known_release") or "versão não reconhecida", title


def last_failure(path: str | None = None) -> str:
    """Return the runtime's actual stop reason without loading an unbounded log into the UI."""
    try:
        with open(path or os.path.join(GENERATED, "boot_stderr.txt"), "rb") as stream:
            stream.seek(0, os.SEEK_END)
            stream.seek(max(0, stream.tell() - 65536))
            lines = stream.read().decode("utf-8", "replace").splitlines()
        for line in reversed(lines):
            if "boot: STOP" in line:
                return line.split(": ", 2)[-1][:400]
            if "host exception" in line:
                return line.strip()[:400]
    except OSError:
        pass
    return "Consulte os logs para identificar a causa."


def setup_steps(nsp: str, nstool: str, keys: str, nca: str, tik: str, redo: bool) -> List[Step]:
    work = os.path.join(USER, "_launcher_setup_" + uuid.uuid4().hex)
    st = status()
    steps: List[Step] = []
    py = [PYTHON, "-m"]
    if redo or not st.exefs or not st.romfs:
        steps.append(("Copiando o NCA de programa e o ticket para fora do NSP", copy_entries(nsp, [nca, tik], work), os.path.join(work, nca)))
    if redo or not st.exefs:
        steps.append(("Extraindo a ExeFS (código do jogo) com o seu nstool",
                      [nstool, "-k", keys, "--tik", os.path.join(work, tik), "--part0", os.path.join(USER, "exefs"), os.path.join(work, nca)],
                      os.path.join(USER, "exefs", "main")))
    if redo or not st.romfs:
        steps.append(("Extraindo a RomFS (dados do jogo, ~7 GB) com o seu nstool",
                      [nstool, "-k", keys, "--tik", os.path.join(work, tik), "--part1", os.path.join(USER, "romfs"), os.path.join(work, nca)],
                      os.path.join(USER, "romfs", "MaterialArchive.arc")))
        steps.append(("Montando user_files/romfs.bin", py + ["dkrecomp.romfs", os.path.join(USER, "romfs"), os.path.join(USER, "romfs.bin"), "--force"],
                      os.path.join(USER, "romfs.bin")))
    steps.append(("Conferindo a ExeFS", py + ["dkrecomp.identify", os.path.join(USER, "exefs"), "--out", os.path.join(ROOT, "reports", "identify_exefs_launcher")], None))
    steps += build_steps(redo=redo)
    if redo or not st.exefs or not st.romfs:
        def cleanup(log):
            # Remove only the two temporary entries created by this setup, never an existing extraction tree.
            from pathlib import Path
            folder = Path(work).resolve()
            if folder.parent != Path(USER).resolve() or not folder.name.startswith("_launcher_setup_"):
                raise ValueError("Pasta temporária fora da área de preparação")
            for name in (nca, tik):
                target = (folder / name).resolve()
                if target.parent != folder:
                    raise ValueError("Entrada temporária inválida")
                target.unlink(missing_ok=True)
            try:
                folder.rmdir()
            except OSError:
                pass
            log("Arquivos temporários da preparação removidos.\n")
        steps.append(("Finalizando a preparação", cleanup, None))
    return steps


def build_steps(redo: bool = False) -> List[Step]:
    ninja = os.path.join(os.path.dirname(PYTHON), "ninja.exe")
    steps: List[Step] = []
    if not os.path.isfile(os.path.join(ROOT, "toolchain", "sdl3", "SDL3.dll")):
        steps.append(("Preparando suporte a Xbox e PlayStation", [PYTHON, os.path.join(ROOT, "tools", "fetch_input_deps.py")],
                      os.path.join(ROOT, "toolchain", "sdl3", "SDL3.dll")))
    if not os.path.isfile(os.path.join(ROOT, "toolchain", "vulkan-headers", "include", "vulkan", "vulkan.h")):
        steps.append(("Preparando suporte gráfico", [PYTHON, os.path.join(ROOT, "tools", "fetch_gfx_deps.py")], None))
    translator = os.path.join(ROOT, "build", "shader_translator", "bin", "dkr_shader_translator.exe")
    identity = os.path.splitext(translator)[0] + ".id"
    translator_stale = not os.path.isfile(translator) or not os.path.isfile(identity)
    if not translator_stale:
        translator_stale = any(os.path.getmtime(os.path.join(ROOT, "tools", "shader_translator", n)) > os.path.getmtime(identity)
                               for n in ("Program.cs", "dkr_shader_translator.csproj"))
    if translator_stale:
        steps.append(("Preparando o tradutor de shaders", [PYTHON, os.path.join(ROOT, "tools", "build_shader_translator.py")],
                      os.path.join(ROOT, "build", "shader_translator", "bin", "dkr_shader_translator.exe")))
    ninja_file = os.path.join(GENERATED, "build.ninja")
    gen_stale = redo or not os.path.isfile(ninja_file) or not all(
        os.path.isfile(os.path.join(GENERATED, n + ".img")) for n in ("main", "sdk", "subsdk0")
    )
    if not gen_stale:
        ninja_time = os.path.getmtime(ninja_file)
        gen_stale = any(
            os.path.getmtime(os.path.join(ROOT, "tools", "dkrecomp", r)) > ninja_time
            for r in ("gen.py", "link.py", "arm64/decoder.py", "recomp/lifter.py")
        )
    if gen_stale:
        steps.append(("Gerando o código C do jogo (~1 min)", [PYTHON, "-m", "dkrecomp.gen", os.path.join(USER, "exefs"), "--out", GENERATED, "--gfx"],
                      ninja_file))
    return steps + [("Compilando (~10 min na primeira vez, segundos depois)", [ninja, "-C", GENERATED, "-j", str(max(2, (os.cpu_count() or 4) - 2))], EXE)]


def run_steps(steps: List[Step], log: Callable[[str], None]) -> bool:
    env = dict(os.environ, PYTHONPATH=os.path.join(ROOT, "tools"), PYTHONIOENCODING="utf-8")
    for desc, cmd, must_exist in steps:
        if desc:
            log(f"\n== {desc}\n")
        if callable(cmd):
            try:
                cmd(log)
            except Exception as e:  # noqa: BLE001 - reported to the user
                log(f"falhou: {e}\n")
                return False
        else:
            try:
                p = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                     encoding="utf-8", errors="replace", creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            except OSError as e:
                log(f"não foi possível executar {cmd[0]}: {e}\n")
                return False
            assert p.stdout
            for line in p.stdout:
                log(line if len(line) < 400 else line[:400] + "…\n")
            if p.wait():
                log(f"falhou (código {p.returncode})\n")
                return False
        if must_exist and not os.path.exists(must_exist):  # nstool reports some failures with exit code 0
            log(f"falhou: {must_exist} não foi criado\n")
            return False
    log("\nPronto.\n")
    return True


def launch() -> subprocess.Popen:
    st = status()
    if not st.ready or st.stale:
        raise RuntimeError("Prepare ou atualize o jogo antes de iniciar.")
    env = dict(os.environ, DKR_CONFIG=CONFIG, DKR_SDL3_PATH=os.path.join(ROOT, "toolchain", "sdl3", "SDL3.dll"))
    with open(os.path.join(GENERATED, "boot_stdout.txt"), "w") as out, open(os.path.join(GENERATED, "boot_stderr.txt"), "w") as err:
        return subprocess.Popen([EXE, ".", "0"], cwd=GENERATED, env=env, stdout=out, stderr=err,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


# ---------------------------------------------------------------- user interface
def main() -> int:
    from .launcher_ui import Launcher
    return Launcher().run()


if __name__ == "__main__":
    raise SystemExit(main())
