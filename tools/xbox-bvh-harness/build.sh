#!/usr/bin/env bash
# Builds the guest harness that loads XboxBvhBuilder.dll through xvalker's title loader.
# Run it as a title EXE: tools/xvalker run <output folder>/bvh_harness.exe --allow-missing
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
mkdir -p "$out"
cd "$out"
x86_64-w64-mingw32-dlltool -d "$here/d3d12_xs.def" -D d3d12_xs.dll -l libd3d12_xs.a
x86_64-w64-mingw32-g++ -std=c++17 -O0 -g -static -o bvh_harness.exe \
    "$here/bvh_harness.cpp" -L. -ld3d12_xs -ld3d12 -ldxgi
echo "bvh-harness: $out/bvh_harness.exe"
