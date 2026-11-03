"""Real SDL3 virtual devices through the same native input code linked into the game. No driver installation."""
import ctypes as ct
import os
from pathlib import Path
import subprocess
import sys
import time

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from dkrecomp.controllers import SDL_PATH  # noqa: E402


class VirtualDesc(ct.Structure):
    _fields_ = [("version", ct.c_uint32), ("type", ct.c_uint16), ("padding", ct.c_uint16),
                ("vendor", ct.c_uint16), ("product", ct.c_uint16), ("naxes", ct.c_uint16),
                ("nbuttons", ct.c_uint16), ("nballs", ct.c_uint16), ("nhats", ct.c_uint16),
                ("ntouchpads", ct.c_uint16), ("nsensors", ct.c_uint16), ("padding2", ct.c_uint16 * 2),
                ("button_mask", ct.c_uint32), ("axis_mask", ct.c_uint32), ("name", ct.c_char_p),
                ("touchpads", ct.c_void_p), ("sensors", ct.c_void_p), ("userdata", ct.c_void_p)] + [
                    (name, ct.c_void_p) for name in ("update", "player", "rumble", "triggers", "led", "effect", "sensor", "cleanup")]


class PadState(ct.Structure):
    _fields_ = [("buttons", ct.c_uint64), ("lx", ct.c_int32), ("ly", ct.c_int32),
                ("rx", ct.c_int32), ("ry", ct.c_int32), ("connected", ct.c_int)]


@pytest.fixture(scope="module")
def bridge(tmp_path_factory):
    if sys.platform != "win32" or not SDL_PATH.is_file():
        pytest.skip("Run tools/fetch_input_deps.py on Windows for real SDL3 integration tests")
    library = tmp_path_factory.mktemp("gamepad") / "gamepad_test.dll"
    subprocess.run([sys.executable, "-m", "ziglang", "cc", "-target", "x86_64-windows-gnu", "-shared",
                    "-Wl,--export-all-symbols", "-O1", "-I", str(ROOT / "runtime/include"),
                    str(ROOT / "runtime/src/gamepad.c"), str(ROOT / "runtime/src/settings.c"),
                    "-o", str(library)], check=True, capture_output=True, timeout=120)
    return library


@pytest.mark.parametrize("vendor,product,name", [(0x045e, 0x02ea, "Xbox One"), (0x054c, 0x09cc, "DualShock 4"),
                                                 (0x054c, 0x0ce6, "DualSense")])
def test_virtual_device_reaches_native_npad_mapping(bridge, vendor, product, name):
    env = dict(os.environ, DKR_SDL3_PATH=str(SDL_PATH), PYTHONIOENCODING="utf-8", DKR_INPUT_BACKEND="auto",
               DKR_STICK_DEADZONE="15", DKR_SWAP_ABXY="false")
    result = subprocess.run([sys.executable, str(Path(__file__).resolve()), str(bridge), str(vendor), str(product), name],
                            env=env, capture_output=True, text=True, encoding="utf-8", timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "PASS" in result.stdout


def exercise(library, vendor, product, name):
    sdl = ct.CDLL(str(SDL_PATH))
    signatures = {
        "Init": ([ct.c_uint32], ct.c_bool), "UpdateGamepads": ([], None), "Quit": ([], None),
        "GetGamepads": ([ct.POINTER(ct.c_int)], ct.POINTER(ct.c_uint32)), "free": ([ct.c_void_p], None),
        "AttachVirtualJoystick": ([ct.POINTER(VirtualDesc)], ct.c_uint32),
        "DetachVirtualJoystick": ([ct.c_uint32], ct.c_bool),
        "OpenJoystick": ([ct.c_uint32], ct.c_void_p), "CloseJoystick": ([ct.c_void_p], None),
        "SetJoystickVirtualButton": ([ct.c_void_p, ct.c_int, ct.c_bool], ct.c_bool),
        "SetJoystickVirtualAxis": ([ct.c_void_p, ct.c_int, ct.c_int16], ct.c_bool),
    }
    for fn, (args, restype) in signatures.items():
        getattr(sdl, "SDL_" + fn).argtypes = args
        getattr(sdl, "SDL_" + fn).restype = restype
    assert sdl.SDL_Init(0x2000)
    count = ct.c_int()
    ids = sdl.SDL_GetGamepads(ct.byref(count))
    sdl.SDL_free(ids)
    assert count.value < 4, "Disconnect extra controllers before this integration test"
    os.environ["DKR_CONTROLLER_INDEX"] = str(count.value)
    os.environ["DKR_SECOND_CONTROLLER_INDEX"] = str(count.value + 1 if count.value < 3 else 4)
    native = ct.CDLL(str(library))
    native.dkr_gamepad_poll.argtypes = [ct.POINTER(PadState)]
    native.dkr_gamepad_poll.restype = ct.c_int
    native.dkr_gamepad_init()
    desc = VirtualDesc()
    desc.version, desc.type = ct.sizeof(desc), 1
    desc.vendor, desc.product = vendor, product
    desc.naxes, desc.nbuttons = 6, 15
    desc.button_mask, desc.axis_mask = 0x7fff, 0x3f
    desc.name = ("DKRecomp virtual " + name).encode()
    instance = sdl.SDL_AttachVirtualJoystick(ct.byref(desc))
    assert instance
    joy = sdl.SDL_OpenJoystick(instance)
    assert joy
    pad = PadState()

    def poll_until(condition):
        end = time.monotonic() + 3
        while time.monotonic() < end:
            native.dkr_gamepad_poll(ct.byref(pad))
            if condition(pad):
                return
            time.sleep(.01)
        raise AssertionError(f"Timed out: connected={pad.connected}, buttons={pad.buttons:#x}, axes={pad.lx},{pad.ly}")

    try:
        poll_until(lambda p: p.connected)
        for source, target in ((0, 1), (1, 0), (2, 3), (3, 2), (9, 6), (10, 7), (6, 10), (11, 13)):
            assert sdl.SDL_SetJoystickVirtualButton(joy, source, True)
            poll_until(lambda p: p.buttons & (1 << target))
            assert sdl.SDL_SetJoystickVirtualButton(joy, source, False)
            poll_until(lambda p: not p.buttons & (1 << target))
        assert sdl.SDL_SetJoystickVirtualAxis(joy, 0, 25000)
        assert sdl.SDL_SetJoystickVirtualAxis(joy, 1, -25000)
        assert sdl.SDL_SetJoystickVirtualAxis(joy, 4, 20000)
        poll_until(lambda p: p.lx > 20000 and p.ly > 20000 and p.buttons & (1 << 8))
        # Two live SDL devices enter separate native player states; unplug must not promote P2 to P1.
        if count.value < 3:
            second = sdl.SDL_AttachVirtualJoystick(ct.byref(desc))
            joy2 = sdl.SDL_OpenJoystick(second)
            assert second and joy2
            native.dkr_gamepad_poll_players.argtypes = [ct.POINTER(PadState)]
            players = (PadState * 2)()
            def poll_pair(predicate):
                end = time.monotonic() + 3
                while time.monotonic() < end:
                    native.dkr_gamepad_poll_players(players)
                    if predicate():
                        return
                    time.sleep(.01)
                raise AssertionError("Two-player routing/hotplug failed")
            try:
                assert sdl.SDL_SetJoystickVirtualButton(joy, 1, True)  # Nintendo A
                assert sdl.SDL_SetJoystickVirtualButton(joy2, 2, True)  # Nintendo Y
                assert sdl.SDL_SetJoystickVirtualAxis(joy2, 0, -30000)
                poll_pair(lambda: players[0].buttons & 1 and players[1].buttons == 8
                          and players[0].lx > 0 and players[1].lx < 0)
                sdl.SDL_CloseJoystick(joy)
                joy = None
                assert sdl.SDL_DetachVirtualJoystick(instance)
                instance = 0
                poll_pair(lambda: not players[0].connected and players[1].buttons == 8)
                instance = sdl.SDL_AttachVirtualJoystick(ct.byref(desc))
                joy = sdl.SDL_OpenJoystick(instance)
                poll_pair(lambda: players[0].connected and players[1].buttons == 8)
                assert players[0].buttons == 0
            finally:
                sdl.SDL_CloseJoystick(joy2)
                sdl.SDL_DetachVirtualJoystick(second)
        sdl.SDL_CloseJoystick(joy)
        joy = None
        assert sdl.SDL_DetachVirtualJoystick(instance)
        instance = 0
        poll_until(lambda p: not p.connected and p.buttons == 0 and p.lx == p.ly == 0)
        print(f"PASS {name}: SDL3 connect, buttons, sticks, trigger, disconnect -> native Npad mapping")
    finally:
        if joy:
            sdl.SDL_CloseJoystick(joy)
        if instance:
            sdl.SDL_DetachVirtualJoystick(instance)
        native.dkr_gamepad_shutdown()


if __name__ == "__main__":
    exercise(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4])
