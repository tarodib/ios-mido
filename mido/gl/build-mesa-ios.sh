#!/bin/bash
# Cross-compile Mesa (gallium freedreno + GLES, no window system, no LLVM) for iOS arm64
# on a macOS runner. Result: static libraries in $WORK/mesa-build, packed to $OUT.
#   build-mesa-ios.sh OUT.tgz
set -euo pipefail
out=$1
here=$(cd "$(dirname "$0")" && pwd)
MESA_TAG=mesa-25.3.6
work=${WORK:-$PWD/mesa-work}
mkdir -p "$work"
cd "$work"

if [ ! -d mesa ]; then
	git clone -q --depth 1 --branch "$MESA_TAG" https://gitlab.freedesktop.org/mesa/mesa.git mesa
	git -C mesa apply "$here/mesa.patch"
fi

SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos --find clang)
CXX=$(xcrun --sdk iphoneos --find clang++)
AR=$(xcrun --sdk iphoneos --find ar)
STRIP=$(xcrun --sdk iphoneos --find strip)
FLAGS="'-arch', 'arm64', '-isysroot', '$SDK', '-miphoneos-version-min=16.0'"
cat > ios-cross.txt <<EOF
[binaries]
c = ['$CC', $FLAGS]
cpp = ['$CXX', $FLAGS]
objc = ['$CC', $FLAGS]
ar = '$AR'
strip = '$STRIP'
pkg-config = 'false'

[built-in options]
c_args = ['-I$here/include', '-I$work/mesa/include', '-I$work/mesa/src']
cpp_args = ['-I$here/include', '-I$work/mesa/include', '-I$work/mesa/src']

[host_machine]
system = 'darwin'
subsystem = 'ios'
kernel = 'xnu'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
EOF

rm -rf mesa-build
meson setup mesa-build mesa --cross-file ios-cross.txt \
	--buildtype=release -Db_ndebug=true -Ddefault_library=static \
	-Dplatforms= -Degl=disabled -Dglx=disabled -Dgbm=disabled \
	-Dgallium-drivers=freedreno -Dvulkan-drivers= -Dfreedreno-kmds=msm \
	-Dllvm=disabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled \
	-Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
	-Dbuild-tests=false -Dshader-cache=disabled -Dperfetto=false \
	-Dgallium-va=disabled -Dgallium-rusticl=false -Dtools= \
	-Dhtml-docs=disabled -Ddisplay-info=disabled -Dteflon=false
# -k 0: unrelated subproject tools (xmllint) may fail; the libraries are checked below.
# Nothing links them without a platform frontend, so name every static library explicitly.
ninja -C mesa-build -k 0 $(ninja -C mesa-build -t targets all | grep -E '\.a: ' | sed 's/:.*//') || true
for lib in src/mesa/libmesa.a src/gallium/drivers/freedreno/libfreedreno.a     src/freedreno/ir3/libfreedreno_ir3.a src/freedreno/drm/libfreedreno_drm.a     src/gallium/winsys/freedreno/drm/libfreedrenowinsys.a; do
	[ -f "mesa-build/$lib" ] || { echo "MISSING $lib"; exit 1; }
done

find mesa-build -name '*.a' | sort
tar -czf "$out" -C "$work" $(cd "$work" && find mesa-build -name '*.a')
ls -la "$out"
