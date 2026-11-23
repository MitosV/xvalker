#!/usr/bin/env bash
# Downloads the official DirectXShaderCompiler Linux release and unpacks it, so the
# corpus tool and the runtime have a `dxc` on Linux.
#
#   tools/fetch-dxc-linux.sh
#
# The release is pinned: the asset's sha256 is checked before it is unpacked.
#
# Environment:
#   XV_DXC_DIR   where the release goes (default: ~/.cache/xvalker/dxc-linux)
set -euo pipefail

# The DirectXShaderCompiler release the shader compiler comes from.
dxc_tag=v1.9.2609
dxc_asset=linux_dxc_2026_09_28.x86_x64.tar.gz
dxc_url="https://github.com/microsoft/DirectXShaderCompiler/releases/download/${dxc_tag}/${dxc_asset}"
# The sha256 of the release asset above.
dxc_sha256=96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1

dir=${XV_DXC_DIR:-$HOME/.cache/xvalker/dxc-linux}

die() { echo "fetch-dxc-linux: $*" >&2; exit 1; }
hash_of() { sha256sum "$1" | cut -d' ' -f1; }

mkdir -p "$dir"
archive="$dir/$dxc_asset"

if [ ! -f "$archive" ]; then
    echo "fetch-dxc-linux: downloading $dxc_asset"
    curl -fsSL -o "$archive.part" "$dxc_url"
    mv "$archive.part" "$archive"
fi
[ "$(hash_of "$archive")" = "$dxc_sha256" ] || die "$dxc_asset does not match its expected sha256"

if [ ! -x "$dir/bin/dxc" ]; then
    echo "fetch-dxc-linux: unpacking"
    rm -rf "$dir/unpacked"
    mkdir -p "$dir/unpacked"
    tar -xzf "$archive" -C "$dir/unpacked"
    [ -x "$dir/unpacked/bin/dxc" ] || die "the release has no bin/dxc executable"
fi

echo "$dir/unpacked/bin/dxc"
