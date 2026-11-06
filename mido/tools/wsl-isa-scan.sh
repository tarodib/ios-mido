#!/bin/bash
# Install numpy (Ubuntu package) and scan the iPhone 8 iOS 16.7.16 dyld cache for ARMv8.1+ instructions
# in real code sections only. Writes report + sample words into hw/.
set -euo pipefail
python3 -c 'import numpy' 2>/dev/null || { DEBIAN_FRONTEND=noninteractive apt-get install -y -qq python3-numpy >/dev/null; }
out=/mnt/c/Users/user/ios-mido/hw
python3 -I /mnt/c/Users/user/ios-mido/tools/isa-scan.py "$HOME/fw/iPhone10,4/20H392__iPhone10,4" "$out/isa-samples.bin" \
	| tee "$out/isa-scan-iPhone10,4-20H392.txt"
