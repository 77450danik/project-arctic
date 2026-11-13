#!/bin/bash
# Disks for trying the stock Rufus in the VM (tests/vm/r*.txt):
#   rufus-apps.img   FAT32, labelled APPS: rufus.exe and an ISO, as the D: of the VM
#   rufus-stick.img  a used 2 GB USB stick: MBR, one FAT32 partition "OLD" with a file
# usage: make-rufus-test-disks.sh RUFUS.EXE IMAGE.ISO
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/out/test-local"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

rufus=$1
iso=$2

img="$OUT/rufus-apps.img"
rm -f "$img"
mb=$(( $(du -cm "$rufus" "$iso" | tail -1 | cut -f1) + 24 ))
truncate -s $((mb + 2))M "$img"
sfdisk -q "$img" <<EOF
label: dos
start=1MiB, type=c
EOF
truncate -s ${mb}M "$TMP/fat"
mkfs.vfat -F 32 -n APPS "$TMP/fat" >/dev/null
MTOOLS_SKIP_CHECK=1 mcopy -i "$TMP/fat" "$rufus" "::rufus.exe"
MTOOLS_SKIP_CHECK=1 mcopy -i "$TMP/fat" "$iso" "::image.iso"
dd if="$TMP/fat" of="$img" bs=1M seek=1 conv=notrunc status=none

img="$OUT/rufus-stick.img"
rm -f "$img"
truncate -s 2G "$img"
sfdisk -q "$img" <<EOF
label: dos
start=1MiB, type=c
EOF
truncate -s 2047M "$TMP/stick"
mkfs.vfat -F 32 -n OLD "$TMP/stick" >/dev/null
echo "old contents" > "$TMP/old.txt"
MTOOLS_SKIP_CHECK=1 mcopy -i "$TMP/stick" "$TMP/old.txt" "::old.txt"
dd if="$TMP/stick" of="$img" bs=1M seek=1 conv=notrunc,sparse status=none

ls -l "$OUT"/rufus-apps.img "$OUT"/rufus-stick.img
