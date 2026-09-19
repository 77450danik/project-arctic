#!/bin/bash
# Regenerates the server side of the Wayland-wire protocols dwmcore speaks.
# The generated files are committed; run this only when a protocol XML changes.
# makedep would build plain .c files as PE code, hence the "makedep unix" pragma.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
XML=${WAYLAND_XML:-$ROOT/out/src/wine/dlls/winewayland.drv}
DST=$ROOT/runtime/wine/modules/dlls/dwmcore

# makedep resolves "quoted" includes among the sources and fails on system
# headers, so the libwayland headers are included with <angle brackets>
fix_includes() { sed -E 's/#include "(wayland-[a-z-]+\.h)"/#include <\1>/'; }

for proto in xdg-shell viewporter; do
    wayland-scanner server-header "$XML/$proto.xml" /dev/stdout | fix_includes > "$DST/$proto-server-protocol.h"
    {
        printf '#if 0\n#pragma makedep unix\n#endif\n\n'
        wayland-scanner private-code "$XML/$proto.xml" /dev/stdout | fix_includes
    } > "$DST/$proto-protocol.c"
done
wayland-scanner --version
ls -l "$DST"
