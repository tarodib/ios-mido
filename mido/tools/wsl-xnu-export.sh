#!/bin/bash
# Copy the patched xnu files the MIDO config touches to the Windows side for reading.
set -eu
src=$HOME/xnu-baseline/xnu
dst=/mnt/c/Users/user/ios-mido/xnu/ref
mkdir -p "$dst"
for f in pexpert/pexpert/arm64/SWEET.h pexpert/arm/pe_fiq.c osfmk/arm64/sleh.c makedefs/MakeInc.def \
         osfmk/arm64/sweet_early.c osfmk/arm64/sweet_early.h iokit/Kernel/arm/SweetCPU.cpp \
         iokit/Kernel/arm/SweetPlatformExpert.cpp iokit/Kernel/arm/SweetFramebuffer.cpp \
         osfmk/arm64/cpu.c osfmk/arm64/machine_routines.c osfmk/arm64/arm_vm_init.c osfmk/arm64/start.s; do
	mkdir -p "$dst/$(dirname "$f")"
	cp "$src/$f" "$dst/$f"
done
grep -n 'SWEET' "$src/osfmk/arm64/start.s" "$src/osfmk/arm64/cpu.c" "$src/osfmk/arm64/machine_routines.c" "$src/osfmk/arm64/arm_vm_init.c" | head -60
wc -l "$dst"/pexpert/arm/pe_fiq.c "$dst"/iokit/Kernel/arm/SweetCPU.cpp "$dst"/osfmk/arm64/sweet_early.c "$dst"/iokit/Kernel/arm/SweetPlatformExpert.cpp
