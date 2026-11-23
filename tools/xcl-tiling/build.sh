#!/usr/bin/env bash
# Builds xcl_tiling.dll (the console texture layout + detiling, C ABI) for 64-bit Windows
# and puts it in the given folder — next to xvboot.exe, where the runtime loads it from.
#
#   tools/xcl-tiling/build.sh <folder>
#
# The crate also builds natively for its tests: cargo test --manifest-path tools/xcl-tiling.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: tools/xcl-tiling/build.sh <folder>}
cargo build --release --manifest-path "$here/Cargo.toml" --target x86_64-pc-windows-gnu
mkdir -p "$out"
cp -f "$here/target/x86_64-pc-windows-gnu/release/xcl_tiling.dll" "$out/"
echo "xcl-tiling: $out/xcl_tiling.dll"