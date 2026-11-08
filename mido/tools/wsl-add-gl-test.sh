#!/bin/bash
# Put midogl-test on a copy of the iOS root image (WSL, as root), padded to a fixed
# 64 MiB so later builds can be written over it in place (see gl-push.ps1).
#   wsl-add-gl-test.sh IN.simg OUT.simg midogl-test EXTENTS.txt
# Flashing the result replaces the iOS root.
set -euo pipefail
in=$1; out=$2; bin=$3; ext=$4
here=$(cd "$(dirname "$0")" && pwd)
raw=$HOME/ios-root-work.img
mnt=/mnt/iosroot
SIZE=$((64*1024*1024))
[ "$(stat -c %s "$bin")" -le "$SIZE" ] || { echo "binary larger than 64 MiB"; exit 1; }
bash "$here/wsl-hfsplus-ko.sh"
simg2img "$in" "$raw"
fsck.hfsplus -fy "$raw" || true
mkdir -p "$mnt"
mount -t hfsplus -o loop,force "$raw" "$mnt"
install -d -o 0 -g 0 -m 755 "$mnt/usr/local/bin"
install -o 0 -g 0 -m 755 "$bin" "$mnt/usr/local/bin/midogl-test"
truncate -s "$SIZE" "$mnt/usr/local/bin/midogl-test"
# force real blocks for the padding too (zero-filled, no holes)
dd if=/dev/zero of="$mnt/usr/local/bin/midogl-test" bs=1M seek=$(( ($(stat -c %s "$bin") + 1048575) / 1048576 )) \
	count=$(( 64 - ($(stat -c %s "$bin") + 1048575) / 1048576 )) conv=notrunc status=none
sync
python3 "$here/add-launchd-job.py" "$mnt/System/Library/xpc/launchd.plist" "$here/org.mido.gl-test.plist"
filefrag -v -b4096 "$mnt/usr/local/bin/midogl-test" | tee "$ext"
sync
umount "$mnt"
img2simg "$raw" "$out"
rm -f "$raw"
ls -la "$out"
