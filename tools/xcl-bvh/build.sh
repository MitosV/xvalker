#!/usr/bin/env bash
# Builds the console's acceleration structures as a library with a C interface: the one the
# runtime loads on Windows (xcl_bvh.dll) and the one the tools use here (libxcl_bvh.so).
#
#   tools/xcl-bvh/build.sh
#
# Environment:
#   XV_BVH_DIR  folder the build goes into (default: the crate's target)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

# the crate has no dependencies: it is XCL's module for the format and nothing else
cargo build --release --target x86_64-pc-windows-gnu
cargo build --release

ls -1 target/x86_64-pc-windows-gnu/release/xcl_bvh.dll target/release/libxcl_bvh.so
