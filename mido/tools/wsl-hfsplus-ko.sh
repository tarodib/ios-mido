#!/bin/bash
# Build and load an out-of-tree hfsplus.ko matching the running WSL2 kernel
# (WSL2 6.18 ships without CONFIG_HFSPLUS_FS; mkiosroot.sh needs to mount HFS+).
# Source: microsoft/WSL2-Linux-Kernel at the tag of the running kernel.
# The module is not installed: it is gone after `wsl --shutdown`; re-run this.
set -euo pipefail
KV=$(uname -r)                       # e.g. 6.18.54.1-microsoft-standard-WSL2
TAG=linux-msft-wsl-${KV%%-*}
if grep -q hfsplus /proc/filesystems; then echo HFSPLUS_LOADED; exit 0; fi
export DEBIAN_FRONTEND=noninteractive
apt-get install -y -q build-essential flex bison bc libelf-dev libssl-dev dwarves cpio >/dev/null
mkdir -p ~/src && cd ~/src
if [ ! -d wsl-kernel ]; then
	git clone -q --depth 1 --branch "$TAG" https://github.com/microsoft/WSL2-Linux-Kernel.git wsl-kernel
fi
cd wsl-kernel
if [ ! -f fs/hfsplus/hfsplus.ko ]; then
	zcat /proc/config.gz > .config
	scripts/config --module CONFIG_HFSPLUS_FS
	make -s olddefconfig
	make -s -j"$(nproc)" modules_prepare
	# No Module.symvers in an unbuilt tree: warn instead of failing on symbol versions
	make -s -j"$(nproc)" M=fs/hfsplus KBUILD_MODPOST_WARN=1 modules
fi
modinfo fs/hfsplus/hfsplus.ko | grep -E 'vermagic|depends'
insmod fs/hfsplus/hfsplus.ko
grep hfsplus /proc/filesystems && echo HFSPLUS_LOADED
