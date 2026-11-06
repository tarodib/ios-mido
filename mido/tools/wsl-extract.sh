#!/bin/bash
# Extract bytes around dyld-cache addresses into hw/ for objdump on the host.
# Usage: wsl-extract.sh 0xADDR [0xADDR...]   (0x80 bytes before, 0x40 after)
d="$HOME/fw/iPhone10,4/20H392__iPhone10,4"
for a in "$@"; do
	python3 -I /mnt/c/Users/user/ios-mido/tools/dsc-extract.py "$d" "$a" 0x80 0x40 "/mnt/c/Users/user/ios-mido/hw/dsc-$a.bin"
done
