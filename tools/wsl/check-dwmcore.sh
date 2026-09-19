#!/bin/bash
# Compiles dwmcore with the real toolchain and Wine headers, without a Wine
# build: catches errors in minutes instead of a full CI round.
# The headers must include our server requests (perl tools/make_requests).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$ROOT/runtime/wine/modules/dlls/dwmcore
WINE_INC=${WINE_INC:-$ROOT/out/src/wine/include}
CONFIG=${WINE_CONFIG:-$ROOT/out/check/wcfg}
OUT=/tmp/check-dwmcore
rm -rf "$OUT"
mkdir -p "$OUT"

for f in kms.c compositor.c xdg-shell-protocol.c viewporter-protocol.c arctic-shell-v1-protocol.c; do
    gcc -c -o "$OUT/${f%.c}.o" -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
        -Wno-missing-field-initializers \
        -I"$SRC" -I"$CONFIG" -I"$WINE_INC" -D__WINESRC__ -DWINE_UNIX_LIB -D_GNU_SOURCE \
        $(pkg-config --cflags wayland-server) "$SRC/$f"
    echo "ok: $f"
done
gcc -shared -o "$OUT/dwmcore.so" "$OUT"/*.o $(pkg-config --libs wayland-server) -Wl,--unresolved-symbols=ignore-all
echo "linked: $(stat -c %s "$OUT/dwmcore.so") bytes"

x86_64-w64-mingw32-gcc -c -o "$OUT/dwmcore-pe.o" -O2 -Wall -Wno-unused-parameter -fno-builtin \
    -I"$SRC" -I"$CONFIG" -I"$WINE_INC" -I"$WINE_INC/msvcrt" -D__WINESRC__ -D_WIN32_WINNT=0x0a00 \
    -D_UCRT -nostdinc -isystem "$(x86_64-w64-mingw32-gcc -print-file-name=include)" "$SRC/dwmcore.c"
echo "ok: dwmcore.c (PE)"
