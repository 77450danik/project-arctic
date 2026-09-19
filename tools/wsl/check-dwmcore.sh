#!/bin/bash
# Compiles dwmcore's unix side with the real toolchain and Wine headers, without
# a Wine build: catches errors in minutes instead of a full CI round.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$ROOT/runtime/wine/modules/dlls/dwmcore
WINE_INC=${WINE_INC:-$ROOT/out/src/wine/include}
CONFIG=${WINE_CONFIG:-$ROOT/out/check/wcfg}
OUT=/tmp/check-dwmcore
mkdir -p "$OUT"

for f in kms.c compositor.c xdg-shell-protocol.c viewporter-protocol.c; do
    gcc -c -o "$OUT/${f%.c}.o" -fPIC -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
        -Wno-missing-field-initializers \
        -I"$SRC" -I"$CONFIG" -I"$WINE_INC" -D__WINESRC__ -DWINE_UNIX_LIB -D_GNU_SOURCE \
        $(pkg-config --cflags wayland-server) "$SRC/$f"
    echo "ok: $f"
done
gcc -shared -o "$OUT/dwmcore.so" "$OUT"/*.o $(pkg-config --libs wayland-server) -Wl,--unresolved-symbols=ignore-all
echo "linked: $(stat -c %s "$OUT/dwmcore.so") bytes"
