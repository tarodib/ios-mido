#!/bin/bash
# Fetch only the dyld_shared_cache (arm64) from Apple's latest iOS 16 IPSW for a device,
# extracted remotely (no full IPSW download). Usage: fetch-dyld.sh iPhone10,4
set -euo pipefail
export PATH="$HOME/tools/bin:$PATH"
export IPSW_APFS_FUSE_PATH="$HOME/tools/bin/apfs-fuse"
dev=${1:-iPhone10,4}
out=~/fw/$dev
mkdir -p "$out"
cd "$out"
~/tools/bin/ipsw download ipsw --device "$dev" --latest --dyld --dyld-arch arm64 --confirm --output "$out" 2>&1 | tail -20
find "$out" -name 'dyld_shared_cache*' -type f -exec ls -l {} \; | head -40
