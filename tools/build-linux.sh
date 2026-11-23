#!/usr/bin/env bash
# Cross-builds XValkyrie for 64-bit Windows on Linux.
#
# The compiler is clang-cl and the Windows SDK and MSVC CRT come from `xwin splat`
# (https://github.com/Jake-Shadle/xwin); see cmake/toolchains/clang-cl-xwin.cmake. The
# DirectX shader compiler is not built here: dxcompiler.dll and dxil.dll are taken from a
# pinned release of the DXC project and checked against their hashes before they are put
# next to the executables.
#
#   tools/build-linux.sh
#
# Environment:
#   XV_BUILD_DIR  folder the build goes into (default: build/linux)
#   XWIN_DIR      folder `xwin splat` wrote (default: ~/.local/share/xwin)
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
build=${XV_BUILD_DIR:-$here/build/linux}
xwin=${XWIN_DIR:-$HOME/.local/share/xwin}
# DXC release the shader compiler comes from, and the hashes of its files
dxc_tag=v1.9.2609
dxc_asset=dxc_2026_09_29.zip
dxc_url="https://github.com/microsoft/DirectXShaderCompiler/releases/download/${dxc_tag}/${dxc_asset}"
dxc_zip_sha256=ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1
dxcompiler_sha256=0689e2fee0a1c9df8662a59680469f24e2a3ad7ce6bbbcc9284f7a83172bfc59
dxil_sha256=64be2368211d257a038a7d4ea6bb3e24c878eb8d588c7b8414fccf759b69c2ac
cache=$build/dxc

die() { echo "build-linux: $*" >&2; exit 1; }
hash_of() { sha256sum "$1" | cut -d' ' -f1; }

[ -d "$xwin/crt/include" ] || die "no Windows SDK in $xwin (run: xwin splat --accept-license)"
mkdir -p "$build" "$cache"

# the shader compiler: downloaded once, then kept
if [ ! -f "$cache/$dxc_asset" ]; then
    echo "build-linux: downloading $dxc_asset"
    curl -fsSL -o "$cache/$dxc_asset.part" "$dxc_url"
    mv "$cache/$dxc_asset.part" "$cache/$dxc_asset"
fi
[ "$(hash_of "$cache/$dxc_asset")" = "$dxc_zip_sha256" ] ||
    die "$dxc_asset does not match its expected hash"
if [ ! -f "$cache/dxcompiler.dll" ] || [ ! -f "$cache/dxil.dll" ]; then
    echo "build-linux: unpacking the shader compiler"
    rm -rf "$cache/unpacked"
    python3 - "$cache/$dxc_asset" "$cache/unpacked" <<'PY'
import os, sys, zipfile
archive, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
with zipfile.ZipFile(archive) as z:
    for name in z.namelist():
        # the archive writes its paths with backslashes
        plain = name.replace("\\", "/")
        if plain in ("bin/x64/dxcompiler.dll", "bin/x64/dxil.dll"):
            target = out + "/" + plain.rsplit("/", 1)[-1]
            with open(target, "wb") as f:
                f.write(z.read(name))
PY
    [ "$(hash_of "$cache/unpacked/dxcompiler.dll")" = "$dxcompiler_sha256" ] ||
        die "dxcompiler.dll does not match its expected hash"
    [ "$(hash_of "$cache/unpacked/dxil.dll")" = "$dxil_sha256" ] ||
        die "dxil.dll does not match its expected hash"
    mv "$cache/unpacked/dxcompiler.dll" "$cache/dxcompiler.dll"
    mv "$cache/unpacked/dxil.dll" "$cache/dxil.dll"
    rm -rf "$cache/unpacked"
fi

echo "build-linux: configuring"
cmake -S "$here" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$here/cmake/toolchains/clang-cl-xwin.cmake" \
    -DXWIN_DIR="$xwin" \
    -DBUILD_TESTING=OFF
echo "build-linux: compiling"
cmake --build "$build" --parallel "$(nproc)"

for dll in dxcompiler.dll dxil.dll; do
    cp -f "$cache/$dll" "$build/$dll"
done
# The Rust parts (tools/*/build.sh: BCPack, XCL's recompiler, tiling, BVH) are optional DLLs the
# runtime loads when they are there; without cargo and the Windows target they are skipped.
if command -v cargo >/dev/null 2>&1 &&
    rustup target list --installed 2>/dev/null | grep -qx x86_64-pc-windows-gnu; then
    for script in "$here"/tools/*/build.sh; do
        [ -f "$script" ] || continue
        echo "build-linux: $(basename "$(dirname "$script")")"
        bash "$script" "$build" || { echo "build-linux: $script failed" >&2; exit 1; }
    done
else
    echo "build-linux: no cargo with x86_64-pc-windows-gnu: the Rust DLLs are not built"
fi
echo "build-linux: done"
ls -1 "$build"/*.exe "$build"/dxcompiler.dll "$build"/dxil.dll
