#!/usr/bin/env bash
# Runs a Windows build of XValkyrie (xvboot.exe / xvalkyrie.exe) on Linux with Proton's
# Wine. Everything this does is in tools/xvalker now; this is the old name of it.
#
#   tools/proton-run.sh xvboot exec <title folder> --allow-missing
#   tools/proton-run.sh xvalkyrie [title]
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
[ $# -ge 1 ] || { echo "usage: $0 xvboot|xvalkyrie [arguments]" >&2; exit 2; }
program=$1
shift
case "$program" in
    xvboot)
        # xvboot takes a command of its own: "exec" runs a title
        [ $# -ge 1 ] || { echo "usage: $0 xvboot exec <title folder> [arguments]" >&2; exit 2; }
        exec "$here/tools/xvalker" run "$@"
        ;;
    xvalkyrie) exec "$here/tools/xvalker" gui "$@" ;;
    *) echo "usage: $0 xvboot|xvalkyrie [arguments]" >&2; exit 2 ;;
esac
