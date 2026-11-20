#!/bin/bash
# The kernel tree for working on Arctic's own drivers (kernel/drivers,
# win32k): built once in full like tools/wsl/build-kernel.sh, then kept, so
# that a change to a driver compiles that driver only.
#
#   kdev.sh setup     unpack, patch, configure and build everything (once, long)
#   kdev.sh driver    copy kernel/drivers/* in again and build them (seconds)
#
# The image's kernel: tools/wsl/build-kernel.sh, which keeps its built tree too.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VER=$(cat "$ROOT/kernel/VERSION")
DEV=/root/arctic/kdev
TREE=$DEV/linux-$VER
export CCACHE_DIR=/root/arctic/ccache-kernel

copy_drivers() {
    for dir in "$ROOT"/kernel/drivers/*/; do
        name=$(basename "$dir")
        rm -rf "$TREE/drivers/$name"
        cp -r "$dir" "$TREE/drivers/$name"
        [ -f "$TREE/drivers/$name/uapi-$name.h" ] &&
            mv "$TREE/drivers/$name/uapi-$name.h" "$TREE/include/uapi/linux/$name.h"
        grep -qx "obj-y += $name/" "$TREE/drivers/Makefile" || echo "obj-y += $name/" >> "$TREE/drivers/Makefile"
    done
}

case "${1:-}" in
setup)
    rm -rf "$DEV"
    mkdir -p "$DEV"
    tar -xf /root/arctic/downloads/linux-$VER.tar.xz -C "$DEV"
    cd "$TREE"
    for patch in "$ROOT"/kernel/patches/*.patch; do
        echo "applying $(basename "$patch")"
        patch -p1 --no-backup-if-mismatch < "$patch"
    done
    python "$ROOT/ci/mkpanic.py" /usr/share/fonts/noto/NotoSans-Light.ttf drivers/gpu/drm/drm_panic_arctic.h
    copy_drivers
    cp /root/arctic/kbuild/linux-$VER/.config .config 2>/dev/null ||
        cp "$(ls /usr/lib/modules/*/build/.config | head -n1)" .config
    cp "$ROOT/kernel/arctic.config" "$DEV/arctic.config"
    scripts/kconfig/merge_config.sh -m .config "$DEV/arctic.config"
    make olddefconfig
    # at the lowest priority: the desktop keeps the processor it needs
    nice -n 19 make -j"$(nproc)" CC="ccache gcc" HOSTCC="ccache gcc" bzImage modules
    echo "kdev: built"
    ;;
resume)
    # setup cut short (the container stops with its last client): go on
    cd "$TREE"
    copy_drivers
    nice -n 19 make -j"$(nproc)" CC="ccache gcc" HOSTCC="ccache gcc" bzImage modules
    echo "kdev: built"
    ;;
driver)
    cd "$TREE"
    copy_drivers
    for dir in "$ROOT"/kernel/drivers/*/; do
        make -j"$(nproc)" CC="ccache gcc" W=1 "drivers/$(basename "$dir")/"
    done
    make -j"$(nproc)" CC="ccache gcc" HOSTCC="ccache gcc" bzImage
    echo "kdev: drivers built"
    ;;
*)
    echo "usage: kdev.sh setup|resume|driver"
    exit 2
    ;;
esac
