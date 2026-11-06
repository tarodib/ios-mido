#!/bin/bash
# Put midogpu-test (and its launchd job) on a copy of the iOS root image (WSL, as root).
#   wsl-add-gpu-test.sh IN.simg OUT.simg midogpu-test
# The result is a full image: flashing it to userdata replaces the iOS root (and the
# state iOS wrote to it since the last flash).
set -euo pipefail
in=$1; out=$2; bin=$3
here=$(cd "$(dirname "$0")" && pwd)
raw=$HOME/ios-root-work.img
mnt=/mnt/iosroot
bash "$here/wsl-hfsplus-ko.sh"
simg2img "$in" "$raw"
fsck.hfsplus -fy "$raw" || true
mkdir -p "$mnt"
mount -t hfsplus -o loop,force "$raw" "$mnt"
install -d -o 0 -g 0 -m 755 "$mnt/usr/local/bin"
install -o 0 -g 0 -m 755 "$bin" "$mnt/usr/local/bin/midogpu-test"
python3 "$here/add-launchd-job.py" "$mnt/System/Library/xpc/launchd.plist" "$here/org.mido.gpu-test.plist"
ls -la "$mnt/usr/local/bin"
sync
umount "$mnt"
img2simg "$raw" "$out"
rm -f "$raw"
ls -la "$out"
