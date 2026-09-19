#!/bin/bash
# Packs out/prefix/drive_c into out/windows.img, an NTFS image that becomes C:.
# Runs on the Ubuntu runner itself (ntfs-3g over FUSE).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/out"
DRIVE_C="$OUT/prefix/drive_c"
IMG="$OUT/windows.img"

sudo apt-get install -y -qq ntfs-3g >/dev/null

used=$(sudo du -sm "$DRIVE_C" | cut -f1)
size=$((used * 13 / 10 + 512))
sudo rm -f "$IMG"
sudo truncate -s "${size}M" "$IMG"
sudo mkntfs -F -Q -q -L ARCTIC -c 4096 "$IMG"

MNT=$(mktemp -d)
sudo ntfs-3g "$IMG" "$MNT"
sudo cp -r "$DRIVE_C/." "$MNT/"

# The case Explorer shows; C: is mounted case-insensitive, so every path still works
rename_case() { sudo mv "$MNT/$1" "$MNT/$1.tmp" && sudo mv "$MNT/$1.tmp" "$MNT/$2"; }
rename_case windows Windows
rename_case Windows/system32 Windows/System32
rename_case Windows/syswow64 Windows/SysWOW64
rename_case users Users

sudo umount "$MNT"
echo "windows.img: ${size} MB for ${used} MB of files"
