#!/bin/bash
# Disk images for trying drive letters in the VM (tools/vm-test.py --disk,
# the plug step of tools/vmscript.py). Output in out/test-local:
#   disk-internal.img  GPT: an EFI system partition, which gets no letter, and
#                      an NTFS partition labelled "Дані" with a file
#   usb-stick.img      MBR: one FAT32 partition labelled "FLASH" with a file
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/out/test-local"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

# part IMAGE START_MIB FILE: the file system in FILE goes in at START_MIB
part() { dd if="$3" of="$1" bs=1M seek="$2" conv=notrunc status=none; }

img="$OUT/disk-internal.img"
rm -f "$img"
truncate -s 104M "$img"
sfdisk -q "$img" <<EOF
label: gpt
start=1MiB, size=33MiB, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, name="EFI system partition"
start=34MiB, size=68MiB, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="Basic data partition"
EOF
truncate -s 33M "$TMP/esp"
mkfs.vfat -F 32 -n SYSTEM "$TMP/esp" >/dev/null
part "$img" 1 "$TMP/esp"
truncate -s 68M "$TMP/data"
mkntfs -F -Q -q -L "Дані" "$TMP/data" 2>/dev/null
echo "Файл на внутрішньому диску" > "$TMP/readme.txt"
ntfscp -q "$TMP/data" "$TMP/readme.txt" "Прочитай.txt"
part "$img" 34 "$TMP/data"

img="$OUT/usb-stick.img"
rm -f "$img"
truncate -s 42M "$img"
sfdisk -q "$img" <<EOF
label: dos
start=1MiB, type=c
EOF
truncate -s 41M "$TMP/stick"
mkfs.vfat -F 32 -n FLASH "$TMP/stick" >/dev/null
echo "Файл на флешці" > "$TMP/stick.txt"
MTOOLS_SKIP_CHECK=1 mcopy -i "$TMP/stick" "$TMP/stick.txt" "::/Flash.txt"
part "$img" 1 "$TMP/stick"

ls -l "$OUT"/disk-internal.img "$OUT"/usb-stick.img
