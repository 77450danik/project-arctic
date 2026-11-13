#!/bin/bash
# Disks for trying the drive letters in the VM (tests/vm/l*.txt), and
# tests/tools/letters.c on the tools disk:
#   stick-a.img, stick-b.img   USB sticks: MBR, one FAT32 partition
#   stick-raw.img              a USB stick: MBR, one FAT32 partition with nothing formatted
#   fixed.img                  a USB hard disk: GPT, one basic data partition, FAT32
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/out/test-local"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT" "$ROOT/out/tests"

x86_64-w64-mingw32-gcc -O1 -s -o "$ROOT/out/tests/letters.exe" "$ROOT/tests/tools/letters.c"
bash "$ROOT/tools/wsl/make-tools-disk.sh"

fat_partition() { # image, label: the file system goes in from the 1 MiB mark
    truncate -s 63M "$TMP/fat"
    mkfs.vfat -F 32 -n "$2" "$TMP/fat" >/dev/null
    dd if="$TMP/fat" of="$1" bs=1M seek=1 conv=notrunc,sparse status=none
    rm -f "$TMP/fat"
}

for name in a b; do
    img="$OUT/stick-$name.img"
    rm -f "$img"
    truncate -s 64M "$img"
    printf 'label: dos\nstart=1MiB, type=c\n' | sfdisk -q "$img"
    fat_partition "$img" "STICK${name^^}"
done

img="$OUT/stick-raw.img"
rm -f "$img"
truncate -s 64M "$img"
printf 'label: dos\nstart=1MiB, type=c\n' | sfdisk -q "$img"

img="$OUT/fixed.img"
rm -f "$img"
truncate -s 128M "$img"
printf 'label: gpt\nstart=1MiB, size=63MiB, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7\n' | sfdisk -q "$img"
fat_partition "$img" FIXED

ls -l "$OUT"/stick-a.img "$OUT"/stick-b.img "$OUT"/stick-raw.img "$OUT"/fixed.img "$OUT"/tools.img
