#!/usr/bin/env bash
# Checks the C ABI of the recompiler: builds the shared library, compiles a small C
# program against src/runtime/modules/xcl_recomp.h and runs it.
#
#   tools/check-xcl-abi.sh
#
# The C program loads the library with dlopen, so it needs nothing but a C compiler
# and the library itself.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
crate=$here/tools/xcl-recomp

cd "$crate"
cargo build --release
lib=$crate/target/release/libxcl_recomp.so
[ -f "$lib" ] || { echo "check-xcl-abi: $lib is missing" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat > "$work/check.c" <<'EOF'
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "xcl_recomp.h"

int main(int argc, char** argv)
{
    void* lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    int32_t (*translate)(const uint8_t*, size_t, uint32_t, XclResult*) = dlsym(lib, "xcl_translate");
    void (*release)(XclResult*) = dlsym(lib, "xcl_free");
    if (!translate || !release) { fprintf(stderr, "the library has no xcl_translate / xcl_free\n"); return 1; }

    XclResult result;
    memset(&result, 0, sizeof(result));
    const uint8_t blob[] = "not a console shader container";
    int32_t code = translate(blob, sizeof(blob) - 1, XCL_FLAG_NONE, &result);
    if (code >= 0) { fprintf(stderr, "expected a failure\n"); return 1; }
    if (!result.error) { fprintf(stderr, "expected an error text\n"); return 1; }
    printf("translate of a foreign blob: %d (%s)\n", code, result.error);
    release(&result);
    if (result.error || result.hlsl || result.profiles) {
        fprintf(stderr, "xcl_free did not clear the result\n");
        return 1;
    }

    int32_t null_code = translate(NULL, 0, XCL_FLAG_NONE, &result);
    if (null_code >= 0) { fprintf(stderr, "expected a failure\n"); return 1; }
    printf("translate of a null blob: %d\n", null_code);
    release(&result);
    release(NULL);
    printf("ok\n");
    return 0;
}
EOF

cc -o "$work/check" "$work/check.c" -I"$here/src/runtime/modules" -ldl
"$work/check" "$lib"
