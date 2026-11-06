#!/bin/bash
# Regenerate xnu/patches/0100-mido-board.patch:
#   baseline = patched xnu-8796 tree from a CI buildtree.tgz (ios-sweet 0001-0081 applied)
#   edits    = mido/xnu/mido_xnu.py (exact-string, count-checked)
# Usage (WSL): mkpatch.sh <buildtree.tgz> <repo root>
set -euo pipefail
tgz=$1
repo=$2
work=$HOME/xnu-mido
rm -rf "$work"
mkdir -p "$work"
tar --no-same-owner -xzf "$tgz" -C "$work" xnu   # CI uid would trip git's safe.directory check
cd "$work/xnu"
git init -q
git -c user.name=mido -c user.email=mido@localhost add -A
git -c user.name=mido -c user.email=mido@localhost commit -q -m baseline
python3 -I "$repo/mido/xnu/mido_xnu.py" .
git add -N . # include new files (mido_input.c)
git diff > "$repo/xnu/patches/0100-mido-board.patch"
git diff --stat
# Prove it applies on the baseline the CI patch step will see.
git reset -q --hard && git clean -qfd   # scratch tree: back to the baseline
git apply --check "$repo/xnu/patches/0100-mido-board.patch" && echo "git apply --check: OK"
