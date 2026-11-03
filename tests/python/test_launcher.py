"""Launcher: settings file round trip, setup planning, and a smoke test of the window (no game data needed)."""
import os
import subprocess
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))

from dkrecomp import launcher  # noqa: E402


def test_settings_round_trip_keeps_values_and_types(tmp_path):
    path = str(tmp_path / "config.local.toml")
    values = dict(launcher.DEFAULTS, display_mode="borderless", render_scale="2.0", fps_limit="120", vsync="off", mute="true")
    launcher.write_settings(values, path)
    text = open(path, encoding="utf-8").read()
    assert 'display_mode = "borderless"' in text and "render_scale = 2.0" in text and "fps_limit = 120" in text
    assert launcher.read_settings(path) == values


def test_unknown_or_missing_file_gives_defaults(tmp_path):
    assert launcher.read_settings(str(tmp_path / "missing.toml")) == launcher.DEFAULTS


def test_bad_config_values_fall_back_without_overwriting_file(tmp_path):
    path = tmp_path / "config.local.toml"
    content = '[display]\nkeep_aspect="maybe"\nwindow_width=-1\n[graphics]\nrender_scale=nan\n[input]\nstick_deadzone=90\n'
    path.write_text(content, encoding="utf-8")
    assert launcher.read_settings(str(path)) == launcher.DEFAULTS
    assert path.read_text(encoding="utf-8") == content


def test_custom_numeric_values_and_input_options_round_trip(tmp_path):
    path = str(tmp_path / "settings.toml")
    values = dict(launcher.DEFAULTS, render_scale="1.25", fps_limit="75", window_width="1366", window_height="768",
                  input_backend="xinput", controller_index="2", stick_deadzone="18", swap_abxy="true", input_background="true")
    launcher.write_settings(values, path)
    assert launcher.read_settings(path) == values


def test_invalid_values_cannot_be_written(tmp_path):
    path = tmp_path / "settings.toml"
    with pytest.raises(ValueError):
        launcher.write_settings(dict(launcher.DEFAULTS, render_scale="1\nunknown = true"), str(path))
    assert not path.exists()


@pytest.mark.parametrize("st", [launcher.Status(True, True, False, False), launcher.Status(True, True, True, True),
                                launcher.Status(True, False, True, False), launcher.Status(True, True, True, False, False)])
def test_play_refuses_missing_or_outdated_build(monkeypatch, st):
    monkeypatch.setattr(launcher, "status", lambda: st)
    monkeypatch.setattr(launcher.subprocess, "Popen", lambda *_a, **_kw: pytest.fail("Game must not start"))
    with pytest.raises(RuntimeError):
        launcher.launch()


def test_stop_reason_is_reported_from_the_end_of_log(tmp_path):
    path = tmp_path / "stderr.txt"
    path.write_text("noise\n" * 50000 + "boot: STOP after 42 SVCs: NVN replay: unsupported clear\nboot: frame 0\n", encoding="utf-8")
    assert launcher.last_failure(str(path)) == "NVN replay: unsupported clear"


def test_every_render_preset_is_a_valid_runtime_scale():
    for _, scale in launcher.RENDER_PRESETS:
        assert 0.5 <= float(scale) <= 4.0


def test_setup_skips_extraction_when_files_exist(monkeypatch):
    monkeypatch.setattr(launcher, "status", lambda: launcher.Status(True, True, True, False))
    steps = launcher.setup_steps("game.nsp", "nstool.exe", "prod.keys", "x.nca", "t.tik", redo=False)
    descriptions = [s[0] for s in steps if s[0]]
    assert not any("NSP" in d or "RomFS" in d or "ExeFS (código" in d for d in descriptions)
    assert any("Compilando" in d for d in descriptions)


def test_build_steps_skips_codegen_when_ninja_is_fresh(monkeypatch):
    monkeypatch.setattr(launcher.os.path, "isfile", lambda _p: True)
    monkeypatch.setattr(launcher.os.path, "getmtime", lambda p: 200.0 if p.endswith("build.ninja") or p.endswith(".id") else 100.0)
    steps = launcher.build_steps(redo=False)
    descriptions = [s[0] for s in steps if s[0]]
    assert not any("Gerando o código C" in d for d in descriptions)
    assert any("Compilando" in d for d in descriptions)


def test_setup_extracts_everything_when_redoing(monkeypatch):
    monkeypatch.setattr(launcher, "status", lambda: launcher.Status(True, True, True, False))
    steps = launcher.setup_steps("game.nsp", "nstool.exe", "prod.keys", "x.nca", "t.tik", redo=True)
    commands = [s[1] for s in steps if isinstance(s[1], list)]
    assert any("--part0" in c for c in commands) and any("--part1" in c for c in commands)
    # the keys are only ever handed to the user's nstool, never to DKRecomp's own modules
    assert all("prod.keys" not in c for c in commands if c[0] != "nstool.exe")


def test_copy_entries_copies_bytes_without_decrypting(tmp_path):
    import struct
    names = [b"a.tik", b"b.nca"]
    data = [b"ticket-bytes", b"x" * 3000]
    strings = b"a.tik\0b.nca\0"
    header = b"PFS0" + struct.pack("<III", 2, len(strings), 0)
    offset = 0
    for d, n in zip(data, names):
        header += struct.pack("<QQII", offset, len(d), strings.index(n), 0)
        offset += len(d)
    nsp = tmp_path / "game.nsp"
    nsp.write_bytes(header + strings + b"".join(data))
    out = tmp_path / "out"
    launcher.copy_entries(str(nsp), ["a.tik", "b.nca"], str(out))(lambda _m: None)
    assert (out / "a.tik").read_bytes() == data[0] and (out / "b.nca").read_bytes() == data[1]


def test_window_opens_and_closes():
    pytest.importorskip("tkinter")
    env = dict(os.environ, DKR_LAUNCHER_SMOKE="1", PYTHONPATH=os.path.join(launcher.ROOT, "tools"))
    r = subprocess.run([sys.executable, "-m", "dkrecomp.launcher"], env=env, capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
