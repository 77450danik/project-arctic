#!/bin/bash
# Packs the same files as the ISO into arctic-usb.img: an MBR disk image with
# one active FAT32 partition, which boots both from UEFI and from BIOS/CSM.
# Sticks are written with this image, byte for byte; the ISO is for discs and
# for QEMU. Runs after ci/build-image.sh, which leaves the file tree in out/iso.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${ARCTIC_OUT:-$ROOT/out}
ISO="$OUT/iso"
IMG="$OUT/arctic-usb.img"
PART_MB=1 # the partition starts at 1 MiB; Limine's BIOS stage lives in the gap

if [ -z "${SKIP_DEPS:-}" ]; then
    source "$ROOT/ci/arch-prep.sh"
    pacman -Syu --noconfirm --needed dosfstools mtools limine util-linux
fi

[ -f "$ISO/arctic/host.sqfs" ] || { echo "run ci/build-image.sh first"; exit 1; }

# the files, plus room for FAT structures and the partition gap
size_mb=$(( $(du -sm --apparent-size "$ISO" | cut -f1) + PART_MB + 48 ))
rm -f "$IMG"
truncate -s "${size_mb}M" "$IMG"

# 0x0c is FAT32 with LBA; firmwares that only boot "active" partitions need the flag
sfdisk --quiet --label dos "$IMG" <<SFDISK
${PART_MB}MiB,,c,*
SFDISK

mformat -i "$IMG@@${PART_MB}M" -F -v ARCTIC ::
mcopy -s -Q -i "$IMG@@${PART_MB}M" "$ISO"/* ::

limine bios-install "$IMG"

ls -l "$IMG"
