#!/bin/bash
# Build the mido iOS root image (ios-sweet tools) from the iPhone10,5 16.7.16 IPSW.
# Needs HFS+ in the WSL kernel: run wsl-hfsplus-ko.sh first (after every wsl --shutdown).
set -euo pipefail
export PATH=$HOME/tools/bin:$PATH
export IPSW_APFS_FUSE_PATH=$HOME/tools/bin/apfs-fuse
R=/mnt/c/Users/user/ios-mido/repo
W=~/fw/ipsw-10,5
I=$W/iPhone_5.5_P3_16.7.16_20H392_Restore.ipsw
cd $W
echo "== extract"
[ -f x/087-86715-047.dmg ] || unzip -o -q "$I" 087-86694-044.dmg 087-86588-047.dmg 087-86715-047.dmg -d x
[ -f x/d211ap.adt ] || ipsw img4 im4p extract x/Firmware/all_flash/DeviceTree.d211ap.im4p -o x/d211ap.adt
ls -la x x/d211ap.adt
grep -q hfsplus /proc/filesystems || { echo "no hfsplus in this kernel: run wsl-hfsplus-ko.sh" >&2; exit 1; }
echo "== mkiosroot"
rm -f root/ios16-root.img root/ios16-root.simg
mkdir -p root
# The repo is checked out on Windows (CRLF): run an LF copy
tr -d '\r' < $R/tools/mkiosroot.sh > /tmp/mkiosroot.sh
bash /tmp/mkiosroot.sh x/087-86694-044.dmg x/087-86588-047.dmg x/087-86715-047.dmg root/ios16-root.img 16G
echo "== fsck"
fsck.hfsplus -fy root/ios16-root.img || true
echo "== tweaks"
mkdir -p /mnt/iosroot
mount -t hfsplus -o loop,force root/ios16-root.img /mnt/iosroot
python3 $R/tools/sweet-root-tweaks.py /mnt/iosroot
df -h /mnt/iosroot
umount /mnt/iosroot
echo "== sparse"
img2simg root/ios16-root.img root/ios16-root.simg
ls -la root
echo MKROOT_DONE
