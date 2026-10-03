#!/bin/bash
# tests/tools/mediatest.c needs the WinRT headers, which Wine's build has and
# zig's mingw does not: built here with the build's headers and combase, into
# out/tests for the tools disk (make-tools-disk.sh)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
W=${WINE_WORK:-/root/arctic/wine}
mkdir -p "$ROOT/out/tests"
x86_64-w64-mingw32-gcc -O2 -s -mwindows -I"$W/build/include" -I"$W/src/include" \
    -o "$ROOT/out/tests/mediatest.exe" "$ROOT/tests/tools/mediatest.c" \
    -L"$W/build/dlls/combase/x86_64-windows" -lcombase
ls -l "$ROOT/out/tests/mediatest.exe"
