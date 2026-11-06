#!/usr/bin/env python3
"""Adjust blacktop/darwin-xnu-build's build.sh for the MIDO kernel.

libfirehose_kernel (os_log's kernel side, from libdispatch) is built by
xcodebuild, not by xnu's makefiles, so it never sees MACHINE_FLAGS. With the
macOS SDK the default arm64 CPU is an Apple core, which emits LSE atomics:
build #6 still had 11 LSE instructions, all in firehose_* functions. Cortex-A53
(ARMv8.0) faults on them. Build only the plain arm64 slice the kernel links,
for Cortex-A53.

  patch_dxb.py dxb/build.sh
"""
import sys

path = sys.argv[1]
text = open(path).read()
old = 'ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"'
new = ('ARCHS="arm64" VALID_ARCHS="arm64" '
       "OTHER_CFLAGS='$(inherited) -mcpu=cortex-a53+crc+crypto'")
n = text.count(old)
if n != 1:
    sys.exit(f'patch_dxb.py: expected 1 libfirehose_kernel ARCHS line, found {n}')
open(path, 'w').write(text.replace(old, new))
print('patch_dxb.py: libfirehose_kernel -> arm64 only, -mcpu=cortex-a53+crc+crypto')
