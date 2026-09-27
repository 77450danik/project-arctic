#!/bin/bash
# Compiles our Wine modules with the real toolchains and Wine headers, without
# a Wine build: catches errors in minutes instead of a full build.
# The headers must include our server requests (perl tools/make_requests).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MODULES=$ROOT/runtime/wine/modules
WINE_INC=${WINE_INC:-$ROOT/out/src/wine/include}
CONFIG=${WINE_CONFIG:-$ROOT/out/check/wcfg}
OUT=/tmp/check-modules
rm -rf "$OUT"
mkdir -p "$OUT"

# unix side: dir, pkg-config packages, sources
unix() {
    local dir=$1 pkgs=$2; shift 2
    for f in "$@"; do
        gcc -c -o "$OUT/$(basename "$dir")-${f%.c}.o" -fPIC -O2 -Wall -Wextra -Wno-unused-parameter \
            -Wno-sign-compare -Wno-missing-field-initializers \
            -I"$MODULES/$dir" -I"$CONFIG" -I"$WINE_INC" -D__WINESRC__ -DWINE_UNIX_LIB -D_GNU_SOURCE \
            $(pkg-config --cflags $pkgs) "$MODULES/$dir/$f"
        echo "ok: $dir/$f"
    done
}

# PE side
pe() {
    local dir=$1; shift
    for f in "$@"; do
        x86_64-w64-mingw32-gcc -c -o "$OUT/$(basename "$dir")-${f%.c}-pe.o" -O2 -Wall -Wno-unused-parameter \
            -fno-builtin -I"$MODULES/$dir" -I"$CONFIG" -I"$WINE_INC" -I"$WINE_INC/msvcrt" -D__WINESRC__ \
            -D_WIN32_WINNT=0x0a00 -D_UCRT -nostdinc -isystem "$(x86_64-w64-mingw32-gcc -print-file-name=include)" \
            "$MODULES/$dir/$f"
        echo "ok: $dir/$f (PE)"
    done
}

unix dlls/dwmcore wayland-server kms.c compositor.c dmabuf.c linux-dmabuf-v1-protocol.c xdg-shell-protocol.c viewporter-protocol.c \
    xdg-output-unstable-v1-protocol.c arctic-shell-v1-protocol.c arctic-display-v1-protocol.c
pe dlls/dwmcore dwmcore.c
unix dlls/winsrv "libinput libudev" libinput.c
pe dlls/winsrv desktop.c rit.c
pe programs/csrss main.c
pe programs/dwm main.c
pe programs/wininit main.c
pe programs/winlogon main.c security.c
pe programs/userinit main.c
pe programs/vktest main.c
pe programs/dispmode main.c
