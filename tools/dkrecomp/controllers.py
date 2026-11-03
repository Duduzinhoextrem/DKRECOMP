"""Live SDL3 gamepad diagnostics for the launcher (no window/video/audio subsystem)."""
from __future__ import annotations

import ctypes as ct
import os
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SDL_PATH = ROOT / "toolchain" / "sdl3" / "SDL3.dll"
BUTTONS = ("A / ×", "B / ○", "X / □", "Y / △", "Back / Share", "Guide / PS", "Start / Options",
           "L3", "R3", "LB / L1", "RB / R1", "↑", "↓", "←", "→")


@dataclass
class Device:
    instance: int
    name: str
    kind: int
    buttons: tuple[bool, ...]
    axes: tuple[int, ...]


class Gamepads:
    """Create, poll, and close on the Tk main thread. Hardware is released before starting the game."""

    def __init__(self, path: str | Path | None = None):
        self.lib = ct.CDLL(str(path or os.environ.get("DKR_SDL3_PATH") or SDL_PATH))
        signatures = {
            "Init": ([ct.c_uint32], ct.c_bool), "Quit": ([], None),
            "SetHint": ([ct.c_char_p, ct.c_char_p], ct.c_bool),
            "SetGamepadEventsEnabled": ([ct.c_bool], None), "UpdateGamepads": ([], None),
            "GetGamepads": ([ct.POINTER(ct.c_int)], ct.POINTER(ct.c_uint32)), "free": ([ct.c_void_p], None),
            "OpenGamepad": ([ct.c_uint32], ct.c_void_p), "CloseGamepad": ([ct.c_void_p], None),
            "GamepadConnected": ([ct.c_void_p], ct.c_bool),
            "GetGamepadName": ([ct.c_void_p], ct.c_char_p), "GetGamepadType": ([ct.c_void_p], ct.c_int),
            "GetGamepadButton": ([ct.c_void_p, ct.c_int], ct.c_bool),
            "GetGamepadAxis": ([ct.c_void_p, ct.c_int], ct.c_int16), "GetError": ([], ct.c_char_p),
        }
        for name, (args, result) in signatures.items():
            function = getattr(self.lib, "SDL_" + name)
            function.argtypes, function.restype = args, result
        for key in (b"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", b"SDL_JOYSTICK_HIDAPI_PS4", b"SDL_JOYSTICK_HIDAPI_PS5"):
            self.lib.SDL_SetHint(key, b"1")
        if not self.lib.SDL_Init(0x2000):
            self.lib.SDL_Quit()
            raise RuntimeError(self.lib.SDL_GetError().decode("utf-8", "replace"))
        self.lib.SDL_SetGamepadEventsEnabled(False)
        self.handles: dict[int, int] = {}
        self.next_scan = 0.0
        self.closed = False

    def poll(self, rescan: bool = False) -> list[Device]:
        if self.closed:
            return []
        sdl = self.lib
        sdl.SDL_UpdateGamepads()
        if rescan or time.monotonic() >= self.next_scan:
            self.next_scan = time.monotonic() + 0.5
            count = ct.c_int()
            ids = sdl.SDL_GetGamepads(ct.byref(count))
            try:
                live = [ids[i] for i in range(count.value)] if ids else []
            finally:
                sdl.SDL_free(ids)
            for instance in list(self.handles):
                if instance not in live:
                    sdl.SDL_CloseGamepad(self.handles.pop(instance))
            for instance in live:
                if instance not in self.handles:
                    handle = sdl.SDL_OpenGamepad(instance)
                    if handle:
                        self.handles[instance] = handle
        devices = []
        for instance, handle in self.handles.items():
            if not sdl.SDL_GamepadConnected(handle):
                continue
            devices.append(Device(instance, (sdl.SDL_GetGamepadName(handle) or b"Gamepad").decode("utf-8", "replace"),
                                  sdl.SDL_GetGamepadType(handle),
                                  tuple(bool(sdl.SDL_GetGamepadButton(handle, b)) for b in range(15)),
                                  tuple(sdl.SDL_GetGamepadAxis(handle, a) for a in range(6))))
        return devices

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        for handle in self.handles.values():
            self.lib.SDL_CloseGamepad(handle)
        self.handles.clear()
        self.lib.SDL_Quit()
