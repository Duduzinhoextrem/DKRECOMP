"""Fetch pinned, permissively licensed graphics headers into the ignored local toolchain directory.

No installer, registry changes or system-wide SDK is needed. Vulkan itself is loaded from the Windows driver.
"""
from __future__ import annotations

import hashlib
import argparse
import json
from pathlib import Path, PurePosixPath
import shutil
import re
import urllib.request
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parent.parent
TAG = "v1.4.321"
COMMIT = "2cd90f9d20df57eac214c148f3aed885372ddcfe"
SHA256 = "cace1d5832fc00287c9b10b27577c41033b2c86434e884eef5c8dcea098d5118"
URL = f"https://codeload.github.com/KhronosGroup/Vulkan-Headers/zip/refs/tags/{TAG}"


def request_url(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "DKRecomp dependency setup"}), timeout=45)


def fetch_validation() -> dict:
    # Khronos publishes Windows binaries as a nightly test tool. Record the exact source commit and digest of
    # this snapshot; it is not linked into the runtime or distributed with generated games.
    with request_url("https://api.github.com/repos/KhronosGroup/Vulkan-ValidationLayers/releases/tags/nightly") as response:
        release = json.load(response)
    commit_match = re.search(r"commit ([0-9a-f]{40})", release["body"])
    if not commit_match:
        raise ValueError("validation release does not identify its source commit")
    commit = commit_match.group(1)
    asset = next(a for a in release["assets"] if a["name"] == "Validation_Layers_Windows_64.zip")
    digest = asset.get("digest", "")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
        raise ValueError("official validation release has no SHA-256 digest")
    expected = digest.removeprefix("sha256:")
    url = asset["browser_download_url"]
    if not url.startswith("https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/"):
        raise ValueError("unexpected validation download origin")
    archive = ROOT / "toolchain" / "downloads" / f"vulkan-validation-{commit}.zip"
    if not archive.exists():
        partial = archive.with_suffix(".zip.part")
        try:
            with request_url(url) as response, partial.open("wb") as output:
                shutil.copyfileobj(response, output)
            if hashlib.sha256(partial.read_bytes()).hexdigest() != expected:
                raise ValueError("validation binary failed SHA-256 verification")
            partial.replace(archive)
        finally:
            partial.unlink(missing_ok=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
        raise ValueError("validation archive does not match the official digest")
    destination = ROOT / "toolchain" / "vulkan-validation"
    destination.mkdir(parents=True, exist_ok=True)
    with ZipFile(archive) as source:
        for name in ("VkLayer_khronos_validation.dll", "VkLayer_khronos_validation.json"):
            (destination / name).write_bytes(source.read(name))
    with request_url(f"https://raw.githubusercontent.com/KhronosGroup/Vulkan-ValidationLayers/{commit}/LICENSE.txt") as response:
        (destination / "LICENSE.txt").write_bytes(response.read())
    info = {"component": "Vulkan-ValidationLayers", "commit": commit, "asset_id": asset["id"],
            "sha256": expected, "source": url, "license": "Apache-2.0", "usage": "development validation only"}
    # The loader treats every .json here as a layer manifest, so use a .txt suffix for our provenance.
    (destination / "source.txt").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    return info


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validation", action="store_true", help="also fetch an official Khronos Windows validation snapshot")
    args = parser.parse_args()
    archive = ROOT / "toolchain" / "downloads" / f"vulkan-headers-{TAG}.zip"
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        partial = archive.with_suffix(".zip.part")
        try:
            request = urllib.request.Request(URL, headers={"User-Agent": "DKRecomp dependency setup"})
            with urllib.request.urlopen(request, timeout=45) as response, partial.open("wb") as output:
                shutil.copyfileobj(response, output)
            if hashlib.sha256(partial.read_bytes()).hexdigest() != SHA256:
                raise ValueError("Vulkan-Headers download failed SHA-256 verification")
            partial.replace(archive)
        finally:
            partial.unlink(missing_ok=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise ValueError(f"Vulkan-Headers archive has changed: {archive}")
    destination = ROOT / "toolchain" / "vulkan-headers"
    files = 0
    with ZipFile(archive) as source:
        if source.comment.decode("ascii") != COMMIT:
            raise ValueError("unexpected Vulkan-Headers source revision")
        for entry in source.infolist():
            parts = PurePosixPath(entry.filename).parts
            if entry.is_dir() or len(parts) < 2:
                continue
            relative = PurePosixPath(*parts[1:])
            if relative.parts[0] not in ("include", "LICENSES", "LICENSE.md"):
                continue
            if relative.is_absolute() or any(part in (".", "..") or ":" in part or "\\" in part for part in relative.parts):
                raise ValueError("invalid dependency archive path")
            path = destination.joinpath(*relative.parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(source.read(entry))
            files += 1
    provenance = {"component": "Vulkan-Headers", "tag": TAG, "commit": COMMIT, "sha256": SHA256,
                  "license": "Apache-2.0 OR MIT", "source": URL, "files": files}
    (destination / "source.json").write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(provenance, indent=2))
    if args.validation:
        print(json.dumps(fetch_validation(), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
