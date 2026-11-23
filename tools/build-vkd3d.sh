#!/usr/bin/env bash
# Builds vkd3d-proton for Windows with the mingw cross compiler and puts d3d12.dll and
# d3d12core.dll next to the executables of the cross build.
#
# One change is made to the source: the check of a resource's vtable accepts the console's
# vtable as well (see cmake/vkd3d.patch). No other check is touched, and none of them is
# left out of the build (no NDEBUG), so a check that fails still stops the program.
#
#   tools/build-vkd3d.sh
#
# Environment:
#   XV_BUILD_DIR    folder the executables are in (default: build/linux)
#   XV_VKD3D_DIR    folder the source and the build go into (default: build/vkd3d)
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
build=${XV_BUILD_DIR:-$here/build/linux}
root=${XV_VKD3D_DIR:-$here/build/vkd3d}
src=$root/vkd3d-proton
out=$root/build
repo=https://github.com/HansKristian-Work/vkd3d-proton.git
commit=2576c0b4

die() { echo "build-vkd3d: $*" >&2; exit 1; }

mkdir -p "$root"
if [ ! -d "$src/.git" ]; then
    echo "build-vkd3d: cloning vkd3d-proton"
    git clone "$repo" "$src"
fi
git -C "$src" fetch --all --tags
git -C "$src" checkout "$commit"
git -C "$src" submodule update --init --recursive

echo "build-vkd3d: patching"
git -C "$src" checkout -- libs/vkd3d/vkd3d_private.h
git -C "$src" apply --whitespace=nowarn "$here/cmake/vkd3d.patch"
cp "$here/cmake/vkd3d-mingw-cross.txt" "$src/build-win64.txt"

echo "build-vkd3d: configuring"
rm -rf "$out"
meson setup "$out" "$src" \
    --cross-file "$src/build-win64.txt" \
    --buildtype=release \
    --default-library=shared \
    -Denable_tests=false
echo "build-vkd3d: compiling"
meson compile -C "$out"

mkdir -p "$build"
for dll in d3d12/d3d12 d3d12core/d3d12core; do
    [ -f "$out/libs/$dll.dll" ] || die "$out/libs/$dll.dll was not built"
    cp -f "$out/libs/$dll.dll" "$build/$(basename "$dll").dll"
done
echo "build-vkd3d: done"
ls -1 "$build"/d3d12.dll "$build"/d3d12core.dll
