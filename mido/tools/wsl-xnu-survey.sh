#!/bin/bash
# Extract the CI build tree (patched xnu-8796) and survey what the mido config must change.
set -eu   # no pipefail: `| head` closing early (SIGPIPE) is expected here
dst=$HOME/xnu-baseline
if [ ! -d "$dst/xnu" ]; then
	mkdir -p "$dst"
	tar -xzf /mnt/c/Users/user/ios-mido/xnu/buildtree-baseline/buildtree.tgz -C "$dst"
fi
cd "$dst/xnu"
echo "== tree"; ls | head -30
echo "== SWEET.h"; wc -l pexpert/pexpert/arm64/SWEET.h
echo "== files using __ARM_16K_PG__ (kernel sources, excluding tests)"
grep -rl '__ARM_16K_PG__' --include='*.[chs]' --include='*.cpp' . | grep -v -e '^./tests' | sort | head -60
echo "== count of #if/#ifdef on __ARM_16K_PG__"
grep -rn '__ARM_16K_PG__' --include='*.[chs]' --include='*.cpp' . | grep -v '^./tests' | wc -l
echo "== 4K page support markers"
grep -rn 'ARM_PGSHIFT\b' osfmk/arm64/proc_reg.h | head -10
grep -rn 'pmap_pt_attr_4k\|pmap_pt_attr_16k\|ARM_4K_TT' osfmk/arm/pmap/*.c osfmk/arm64/*.h 2>/dev/null | head -12
echo "== GIC code paths"
grep -rln 'HAS_GIC_V3\|GICD_CTLR' --include='*.[chs]' . | head -20
echo "== SWEET-gated files"
grep -rl 'defined(SWEET)\|defined (SWEET)\|ifdef SWEET' --include='*.[chs]' --include='*.cpp' . | sort | head -60
