"""Install the pinned official SDL3 gamepad DLL locally, after SHA-256 verification."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import urllib.request
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parent.parent
VERSION = "3.4.18"
SHA256 = "75c2c0fc74e7d1206aaedc22893a35887332f4f85f5c8b74fddc10d177985f2c"
URL = f"https://github.com/libsdl-org/SDL/releases/download/release-{VERSION}/SDL3-{VERSION}-win32-x64.zip"


def main() -> int:
    archive = ROOT / "toolchain" / "downloads" / f"SDL3-{VERSION}-win32-x64.zip"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        partial = archive.with_suffix(".zip.part")
        try:
            request = urllib.request.Request(URL, headers={"User-Agent": "DKRecomp dependency setup"})
            with urllib.request.urlopen(request, timeout=45) as source, partial.open("wb") as output:
                shutil.copyfileobj(source, output)
            if hashlib.sha256(partial.read_bytes()).hexdigest() != SHA256:
                raise ValueError("SDL3 download failed SHA-256 verification")
            partial.replace(archive)
        finally:
            partial.unlink(missing_ok=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise ValueError("SDL3 archive failed SHA-256 verification")
    destination = ROOT / "toolchain" / "sdl3"
    destination.mkdir(parents=True, exist_ok=True)
    with ZipFile(archive) as source:
        for name in ("SDL3.dll", "LICENSE.txt"):
            match = next(n for n in source.namelist() if n.rsplit("/", 1)[-1] == name)
            data = source.read(match)
            target = destination / name
            if not target.exists() or target.read_bytes() != data:
                target.write_bytes(data)
    (destination / "source.json").write_text(json.dumps({
        "component": "SDL3", "version": VERSION, "sha256": SHA256, "source": URL, "license": "zlib",
    }, indent=2) + "\n", encoding="utf-8")
    print(f"SDL3 {VERSION}: verified, installed in {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
