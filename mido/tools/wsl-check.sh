#!/bin/bash
# Verify the Linux-side toolchain for the mido port.
for t in aarch64-linux-gnu-gcc mkfs.hfsplus fsck.hfsplus img2simg mkbootimg dtc cmake git python3; do
	printf '%-22s ' "$t"; command -v "$t" || echo MISSING
done
for t in ipsw apfs-fuse; do
	printf '%-22s ' "$t"; command -v "$t" || echo "MISSING (built separately)"
done
df -h "$HOME" | tail -1
