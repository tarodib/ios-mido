#!/bin/bash
# Inspect the built iOS root image (read-only mount).
set -u
grep -q hfsplus /proc/filesystems || { echo "no hfsplus: run wsl-hfsplus-ko.sh"; exit 1; }
IMG=~/fw/ipsw-10,5/root/ios16-root.img
mkdir -p /mnt/iosro
mountpoint -q /mnt/iosro || mount -t hfsplus -o loop,ro "$IMG" /mnt/iosro
cd /mnt/iosro
for d in System/Library/Caches/com.apple.dyld System/Cryptexes/OS/System/Library/Caches/com.apple.dyld \
         private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld System/Volumes/Preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld; do
	echo "== $d"; ls -la "$d" 2>&1 | head -8
done
echo "== /System/Cryptexes"; ls -la System/Cryptexes 2>&1
echo "== usr/lib dyld + libSystem"; ls -la usr/lib/dyld usr/lib/libSystem.B.dylib 2>&1
echo "== cache header"; f=$(ls System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64* 2>/dev/null | grep -v '\.[0-9]' | head -1); echo "$f"; [ -n "$f" ] && head -c 16 "$f" | od -c | head -2
umount /mnt/iosro
