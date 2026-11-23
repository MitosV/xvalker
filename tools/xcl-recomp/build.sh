#!/usr/bin/env bash
# Builds xcl_recomp.dll (XCL's shader recompiler behind a C ABI, see
# src/runtime/modules/xcl_recomp.h) and copies it to the given folder.
#
#   tools/xcl-recomp/build.sh [output-directory]    (default: build/linux)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/../../build/linux}
cd "$here"
cargo build --release --lib --target x86_64-pc-windows-gnu
mkdir -p "$out"
cp -f "$here/target/x86_64-pc-windows-gnu/release/xcl_recomp.dll" "$out/xcl_recomp.dll"
echo "xcl-recomp: $out/xcl_recomp.dll"
