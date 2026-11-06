#!/bin/bash
# grep the patched baseline xnu tree (kernel sources only).
# Usage: wsl-xnu-grep.sh 'pat1,pat2,...'   (comma-separated; wsl.exe mangles '|')
cd "$HOME/xnu-baseline/xnu"
args=()
IFS=',' read -ra pats <<<"$1"
for p in "${pats[@]}"; do args+=(-e "$p"); done
grep -rnE "${args[@]}" --include='*.c' --include='*.cpp' --include='*.h' --include='*.s' . | grep -v '^./tests' | cut -c1-180
