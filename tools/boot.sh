#!/usr/bin/env bash
# Regenerate (only changed files are rewritten), rebuild and run one boot attempt of the game.
# Usage: tools/boot.sh [seconds] [--gfx|--no-gfx] (graphics by default; see docs/TESTING.md)
set -u
cd "$(dirname "$0")/.."
OUT=generated/dkctf_v0
mkdir -p "$OUT"
GEN_FLAGS=(--gfx)
if [[ "${2:-}" == "--no-gfx" ]]; then GEN_FLAGS=()
elif [[ -n "${2:-}" && "${2:-}" != "--gfx" ]]; then echo "usage: tools/boot.sh [seconds] [--gfx|--no-gfx]"; exit 2; fi
PYTHONIOENCODING=utf-8 PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.gen user_files/exefs --out "$OUT" "${GEN_FLAGS[@]}" \
    > "$OUT/gen_stdout.txt" 2> "$OUT/gen_stderr.txt" || { echo "generation failed"; tail "$OUT/gen_stderr.txt"; exit 1; }
.venv/Scripts/ninja -C "$OUT" -j 12 > "$OUT/ninja.log" 2>&1 || { echo "build failed"; grep -m5 error "$OUT/ninja.log"; exit 1; }
if [[ -n "${DKR_GFX_CAPTURE:-}" && "${DKR_GFX_CAPTURE}" != /* && "${DKR_GFX_CAPTURE}" != [A-Za-z]:* ]]; then
    export DKR_GFX_CAPTURE="$(pwd)/${DKR_GFX_CAPTURE}"
fi
if [[ -d "toolchain/vulkan-validation" && -z "${VK_LAYER_PATH:-}" ]]; then
    export VK_LAYER_PATH="$(cygpath -w "$(pwd)/toolchain/vulkan-validation")"
fi
cd "$OUT" && timeout $(( ${1:-60} + 30 )) ./dkctf.exe . "${1:-60}" > boot_stdout.txt 2> boot_stderr.txt
echo "boot exit=$?"
