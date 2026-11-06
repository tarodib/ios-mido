#!/bin/bash
# Disassemble a few instructions around addresses in the iPhone 8 dyld cache (symbolicated).
# Usage: wsl-disass.sh 0xADDR [0xADDR...]
export PATH="$HOME/tools/bin:$PATH"
c="$HOME/fw/iPhone10,4/20H392__iPhone10,4/dyld_shared_cache_arm64"
for a in "$@"; do
	echo "=== $a"
	ipsw dyld a2s "$c" "$a" 2>/dev/null | tail -1
	ipsw dyld disass "$c" --vaddr "$a" --count 3 2>/dev/null | grep -v '^\s*$' | head -6
done
