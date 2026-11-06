#!/bin/bash
# grep xnu build configuration (config/, makedefs/, Makefiles) in the patched baseline tree.
# Usage: wsl-xnu-grep-cfg.sh 'pat1,pat2'
cd "$HOME/xnu-baseline/xnu"
args=()
IFS=',' read -ra pats <<<"$1"
for p in "${pats[@]}"; do args+=(-e "$p"); done
grep -rnE "${args[@]}" config makedefs --include='MASTER*' --include='*.def' --include='*.mk' --include='Makefile*' --include='*.template' --include='files*' 2>/dev/null | cut -c1-200
grep -rnE "${args[@]}" config/MASTER* 2>/dev/null | cut -c1-200
