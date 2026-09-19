#!/bin/bash
# Builds the ISO locally in WSL: the kernel from the last CI ISO (out/arctic.iso
# on G:, fetched by tools/get-iso.ps1), Wine from tools/wsl/build-wine.sh,
# everything else from this checkout. Output: out/arctic-local.iso on G:.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=/root/arctic/out
export ARCTIC_OUT=$OUT SKIP_DEPS=1

if [ ! -f "$OUT/kernel/vmlinuz" ]; then
    echo "taking the kernel from $ROOT/out/arctic.iso"
    rm -rf "$OUT/kernel" /tmp/kernel-from-iso
    mkdir -p "$OUT/kernel/lib" /tmp/kernel-from-iso
    xorriso -osirrox on -indev "$ROOT/out/arctic.iso" \
        -extract /arctic/vmlinuz "$OUT/kernel/vmlinuz" \
        -extract /arctic/host.sqfs /tmp/kernel-from-iso/host.sqfs >/dev/null 2>&1
    unsquashfs -q -d /tmp/kernel-from-iso/root /tmp/kernel-from-iso/host.sqfs usr/lib/modules
    mv /tmp/kernel-from-iso/root/usr/lib/modules "$OUT/kernel/lib/modules"
    rm -rf /tmp/kernel-from-iso
fi

bash "$ROOT/ci/build-rootfs.sh"
bash "$ROOT/ci/make-windows-img.sh"
bash "$ROOT/ci/build-image.sh"
cp "$OUT/arctic.iso" "$ROOT/out/arctic-local.iso"
ls -l "$ROOT/out/arctic-local.iso"
