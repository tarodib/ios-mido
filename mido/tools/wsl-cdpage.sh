#!/bin/bash
umount /mnt/iosro 2>/dev/null
mount -t hfsplus -o loop,ro ~/fw/ipsw-10,5/root/ios16-root.img /mnt/iosro || exit 1
D=/mnt/iosro/System/Library/Caches/com.apple.dyld
ls "$D" | tr '\n' ' '; echo
python3 /mnt/c/Users/user/ios-mido/tools/cache-cdpage.py "$D"/dyld_shared_cache_arm64 "$D"/dyld_shared_cache_arm64.01 "$D"/dyld_shared_cache_arm64.02
umount /mnt/iosro
