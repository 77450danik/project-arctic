#!/bin/bash
# Packs out/prefix/drive_c into out/windows.img, an NTFS image that becomes C:.
# Runs on the Ubuntu runner (ntfs-3g over FUSE), or as root in the local WSL distro.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${ARCTIC_OUT:-$ROOT/out}
DRIVE_C="$OUT/prefix/drive_c"
IMG="$OUT/windows.img"

SUDO=sudo
[ "$(id -u)" = 0 ] && SUDO=
command -v mkntfs >/dev/null || $SUDO apt-get install -y -qq ntfs-3g >/dev/null

used=$($SUDO du -sm "$DRIVE_C" | cut -f1)
size=$((used * 13 / 10 + 512))
$SUDO rm -f "$IMG"
$SUDO truncate -s "${size}M" "$IMG"
$SUDO mkntfs -F -Q -q -L ARCTIC -s 4096 -c 4096 "$IMG"

MNT=$(mktemp -d)
$SUDO ntfs-3g "$IMG" "$MNT"
$SUDO cp -r "$DRIVE_C/." "$MNT/"

# The case Explorer shows; C: is mounted case-insensitive, so every path still works
rename_case() { $SUDO mv "$MNT/$1" "$MNT/$1.tmp" && $SUDO mv "$MNT/$1.tmp" "$MNT/$2"; }
rename_case windows Windows
rename_case Windows/system32 Windows/System32
rename_case Windows/syswow64 Windows/SysWOW64
rename_case users Users

$SUDO umount "$MNT"
echo "windows.img: ${size} MB for ${used} MB of files"
