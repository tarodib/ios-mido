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

# ---- midogl: the bridge, the context layer and the GLES triangle test ----
M=$work/mesa; B=$work/mesa-build
python3 -I - "$B/compile_commands.json" > mesa-defines.rsp <<'PY'
import json, shlex, sys
cc = json.load(open(sys.argv[1]))
e = [x for x in cc if x["file"].endswith("state_tracker/st_manager.c")][0]
print("\n".join(shlex.quote(a) for a in shlex.split(e["command"]) if a.startswith("-D")))
PY
IOSCC="xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0"
INC="-I$here -I$here/include -I$M/include -I$M/src -I$M/src/mesa -I$M/src/gallium/include -I$M/src/gallium/auxiliary -I$M/src/gallium/drivers -I$M/src/gallium/winsys -I$B/src -I$B/src/mesa"
mkdir -p out
for f in midogl midogl_drm midogl-test; do
	$IOSCC -O2 -Wall @mesa-defines.rsp $INC -c "$here/$f.c" -o "out/$f.o"
done
LIBS=$(find mesa-build -name '*.a' | grep -v -E 'libxml2|libarchive|xmllint')
xcrun --sdk iphoneos clang++ -arch arm64 -miphoneos-version-min=16.0 -dead_strip \
	-o out/midogl-test out/midogl-test.o out/midogl.o out/midogl_drm.o \
	$LIBS -framework IOKit -framework CoreFoundation -lc++ -lz
codesign -f -s - out/midogl-test
ls -la out/midogl-test
cp out/midogl-test "$(dirname "$out")/midogl-test"
tar -czf "$out" -C "$work" $(cd "$work" && find mesa-build -name '*.a')
ls -la "$out"
