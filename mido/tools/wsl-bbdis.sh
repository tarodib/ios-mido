#!/bin/bash
# Disassemble around an address in a binary from the iOS root image.
#   wsl-bbdis.sh /usr/libexec/backboardd 0x10001d7c0 60
export PATH=$HOME/tools/bin:$PATH
BIN=$1 ADDR=$2 N=${3:-60}
mkdir -p /mnt/iosro ~/fw/bin
mountpoint -q /mnt/iosro || mount -t hfsplus -o loop,ro ~/fw/ipsw-10,5/root/ios16-root.img /mnt/iosro || exit 1
out=~/fw/bin/$(basename "$BIN")
cp "/mnt/iosro$BIN" "$out"
umount /mnt/iosro
ipsw macho disass "$out" --vaddr "$ADDR" --count "$N" 2>&1 | head -"$((N+20))"
