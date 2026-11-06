#!/bin/bash
# Build/fetch apfs-fuse and ipsw into ~/tools (each in its own directory).
set -euo pipefail
mkdir -p ~/tools/src ~/tools/bin
cd ~/tools/src
if [ ! -x ~/tools/bin/apfs-fuse ]; then
	rm -rf apfs-fuse
	git clone -q --recursive https://github.com/sgan81/apfs-fuse.git
	git -C apfs-fuse log -1 --format='apfs-fuse %h %cd'
	cmake -S apfs-fuse -B apfs-fuse/build -DCMAKE_BUILD_TYPE=Release >/dev/null
	make -C apfs-fuse/build -j"$(nproc)" >/dev/null
	cp apfs-fuse/build/apfs-fuse apfs-fuse/build/apfsutil ~/tools/bin/
fi
if [ ! -x ~/tools/bin/ipsw ]; then
	# Pick a release at least two weeks old.
	url=$(curl -fsSL https://api.github.com/repos/blacktop/ipsw/releases?per_page=10 | python3 -c '
import json, sys, datetime as d
now = d.datetime.now(d.timezone.utc)
for r in json.load(sys.stdin):
    t = d.datetime.fromisoformat(r["published_at"].replace("Z", "+00:00"))
    if r["prerelease"] or (now - t).days < 14: continue
    for a in r["assets"]:
        n = a["name"]
        if n.endswith("_linux_x86_64.tar.gz") and n.startswith("ipsw_"):
            print(a["browser_download_url"]); sys.exit()
')
	echo "ipsw: $url"
	rm -rf ipsw-dl && mkdir ipsw-dl
	curl -fsSL "$url" -o ipsw-dl/ipsw.tgz
	tar -xzf ipsw-dl/ipsw.tgz -C ipsw-dl
	cp "$(find ipsw-dl -type f -name ipsw | head -1)" ~/tools/bin/ipsw
fi
~/tools/bin/ipsw version 2>/dev/null | head -2 || true
ls -l ~/tools/bin
