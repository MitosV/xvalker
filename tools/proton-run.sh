#!/usr/bin/env bash
# Runs a Windows build of XValkyrie (xvboot.exe / xvalkyrie.exe) on Linux with Proton's Wine.
# Direct3D 11 goes to DXVK and Direct3D 12 to vkd3d-proton, both shipped with Proton.
#
#   tools/proton-run.sh xvboot exec <title folder> --allow-missing
#   tools/proton-run.sh xvalkyrie [title]
#
# Proton's own launcher (`proton run`) goes through steam.exe, which drops the program's
# standard output; this script uses Proton only to make the prefix, then starts Proton's
# wine itself with the overrides Proton would set.
#
# XV_BIN      folder with xvboot.exe, xvalkyrie.exe, dxcompiler.dll and dxil.dll
#             (default: build/linux of this repository)
# XV_PROTON   the Proton folder (default: Proton - Experimental of the Steam library)
# XV_PREFIX   the compat data folder Proton keeps its prefix in
#             (default: ~/.cache/xvalkyrie/proton-prefix)
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
bin=${XV_BIN:-$here/build/linux}
steam=${STEAM_COMPAT_CLIENT_INSTALL_PATH:-$HOME/.steam/steam}
proton=${XV_PROTON:-$steam/steamapps/common/Proton - Experimental}
prefix=${XV_PREFIX:-$HOME/.cache/xvalkyrie/proton-prefix}
[ $# -ge 1 ] || { echo "usage: $0 xvboot|xvalkyrie [arguments]" >&2; exit 2; }
program=$1
shift
if [ ! -f "$prefix/pfx/system.reg" ]; then
    mkdir -p "$prefix"
    STEAM_COMPAT_CLIENT_INSTALL_PATH=$steam STEAM_COMPAT_DATA_PATH=$prefix \
        "$proton/proton" run wineboot -u
fi
for dll in dxcompiler.dll dxil.dll; do
    [ -f "$bin/$dll" ] || echo "note: no $dll in $bin; shaders fall back to d3dcompiler_47" >&2
done
export WINEPREFIX=$prefix/pfx
# what Proton sets for DXVK and vkd3d-proton (their DLLs are in the prefix's system32)
export WINEDLLOVERRIDES="d3d11,d3d10core,dxgi,d3d12,d3d12core,d3d9,d3d8=n;${WINEDLLOVERRIDES:-}"
export WINEDEBUG=${WINEDEBUG:--all}
export WINEESYNC=1 WINEFSYNC=1
# Linux paths become Z: paths for the Windows side
args=()
for arg in "$@"; do
    if [[ $arg == /* && -e $arg ]]; then
        args+=("Z:${arg//\//\\}")
    else
        args+=("$arg")
    fi
done
cd "$bin"
status=0
"$proton/files/bin/wine" "./$program.exe" "${args[@]}" || status=$?
"$proton/files/bin/wineserver" -w
exit $status
