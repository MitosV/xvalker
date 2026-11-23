#!/usr/bin/env bash
# Builds bcpack.dll for xvalker and copies it next to the executable.
#
#   tools/bcpack/build.sh [output-directory]
#
# Default output directory: build/linux (the folder tools/build-linux.sh uses).
# The DLL is not built by tools/build-linux.sh; run this script after (or
# before) that build and it will be found next to xvalkyrie.exe.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/../../build/linux}
cd "$here"
cargo build --release --target x86_64-pc-windows-gnu
mkdir -p "$out"
cp -f "$here/target/x86_64-pc-windows-gnu/release/bcpack.dll" "$out/bcpack.dll"
echo "bcpack: $out/bcpack.dll"
