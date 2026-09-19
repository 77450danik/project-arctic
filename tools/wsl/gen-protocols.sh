#!/bin/bash
# Regenerates the server side of the Wayland-wire protocols dwmcore speaks.
# The generated files are committed; run this only when a protocol XML changes.
# makedep would build plain .c files as PE code, hence the "makedep unix" pragma.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
XML=${WAYLAND_XML:-$ROOT/out/src/wine/dlls/winewayland.drv}
DST=$ROOT/runtime/wine/modules/dlls/dwmcore

for proto in xdg-shell viewporter; do
    wayland-scanner server-header "$XML/$proto.xml" "$DST/$proto-server-protocol.h"
    {
        printf '#if 0\n#pragma makedep unix\n#endif\n\n'
        wayland-scanner private-code "$XML/$proto.xml" /dev/stdout
    } > "$DST/$proto-protocol.c"
done
wayland-scanner --version
ls -l "$DST"
