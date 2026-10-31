#!/usr/bin/env bash
# Regenerate (only changed files are rewritten), rebuild and run one boot attempt of the game.
# Usage: tools/boot.sh [seconds]   (run from the repository root; needs user_files/exefs, see docs/TESTING.md)
set -u
cd "$(dirname "$0")/.."
OUT=generated/dkctf_v0
PYTHONIOENCODING=utf-8 PYTHONPATH=tools .venv/Scripts/python -m dkrecomp.gen user_files/exefs --out "$OUT" \
    > "$OUT/gen_stdout.txt" 2> "$OUT/gen_stderr.txt" || { echo "generation failed"; tail "$OUT/gen_stderr.txt"; exit 1; }
.venv/Scripts/ninja -C "$OUT" -j 12 > "$OUT/ninja.log" 2>&1 || { echo "build failed"; grep -m5 error "$OUT/ninja.log"; exit 1; }
cd "$OUT" && timeout "${1:-120}" ./dkctf.exe . > boot_stdout.txt 2> boot_stderr.txt
echo "boot exit=$?"
