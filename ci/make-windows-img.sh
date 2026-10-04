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
# Room for programs installed in a session, the Steam client among them (its
# CEF alone unpacks to about 1 GB). The empty part costs nothing: squashfs
# keeps it sparse, and what is written goes to zram.
size=$((used * 13 / 10 + 3072))
$SUDO rm -f "$IMG"
$SUDO truncate -s "${size}M" "$IMG"
# 512-byte sectors, as sticks and disks have them: Windows does not mount an
# NTFS whose sector size differs from the disk's, and on a stick this image
# is C:. The initrd gives the live-mode snapshot 512-byte blocks to match.
$SUDO mkntfs -F -Q -q -L ARCTIC -s 512 -c 4096 "$IMG"

MNT=$(mktemp -d)
$SUDO ntfs-3g "$IMG" "$MNT"
$SUDO cp -r "$DRIVE_C/." "$MNT/"

# The case Explorer shows; C: is mounted case-insensitive, so every path still works
rename_case() { $SUDO mv "$MNT/$1" "$MNT/$1.tmp" && $SUDO mv "$MNT/$1.tmp" "$MNT/$2"; }
rename_case windows Windows
rename_case Windows/system32 Windows/System32
rename_case Windows/syswow64 Windows/SysWOW64
rename_case Windows/resources Windows/Resources
rename_case Windows/Resources/themes Windows/Resources/Themes
rename_case users Users

$SUDO umount "$MNT"
echo "windows.img: ${size} MB for ${used} MB of files"
