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

# standard protocols come from Wine's tree; ours (arctic-*) and those Wine does not
# carry (linux-dmabuf-v1, from wayland-protocols 1.49) live next to dwmcore
for proto in xdg-shell viewporter xdg-output-unstable-v1 linux-dmabuf-v1 arctic-shell-v1 arctic-display-v1; do
    src=$XML/$proto.xml
    case $proto in arctic-*|linux-dmabuf-v1) src=$DST/$proto.xml ;; esac
    wayland-scanner server-header "$src" /dev/stdout | fix_includes > "$DST/$proto-server-protocol.h"
    {
        printf '#if 0\n#pragma makedep unix\n#endif\n\n'
        wayland-scanner private-code "$src" /dev/stdout | fix_includes
    } > "$DST/$proto-protocol.c"
done
wayland-scanner --version
ls -l "$DST"
