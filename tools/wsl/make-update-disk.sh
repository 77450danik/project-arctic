#!/bin/bash
# A FAT32 disk image holding Update\, the update make-usb-img.sh leaves in
# out/arctic-update (this build, installable over a stick): vm-test.py
# --disk attaches it and tests/vm/p5-stage-update.txt copies it onto C:.
# usage: make-update-disk.sh <image>   (run as root in arctic-build)
set -euo pipefail

IMG=${1:?usage: make-update-disk.sh <image>}
SRC=/root/arctic/out/arctic-update
[ -f "$SRC/Update/ready" ] || { echo "no $SRC: run tools/wsl/build-image.sh first"; exit 1; }

size_mb=$(( $(du -sm --apparent-size "$SRC" | cut -f1) + 64 ))
rm -f "$IMG"
truncate -s "${size_mb}M" "$IMG"
mkfs.fat -F 32 -n UPDATE "$IMG" >/dev/null
mcopy -s -Q -i "$IMG" "$SRC/Update" ::
ls -l "$IMG"
