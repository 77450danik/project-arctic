#!/bin/bash
# Makes arctic-usb.img, the disk image a stick is written with, byte for byte
# (dd, Rufus in DD mode). It boots like an installed system: MBR, so both
# from UEFI and from BIOS/CSM, and two partitions, as Windows To Go has them:
#
#   1  EFI system partition, FAT32, 500 MB: shim, Limine, the kernel and the
#      initrd (EFI\Arctic), ARCTIC.cer. Firmware reads nothing but FAT.
#      Windows gives it no letter.
#   2  C:, NTFS: windows.img with the host in Windows\System32\Host. It is
#      written small and grows over the whole stick on its first start.
#
# The initrd finds C: by its PARTUUID, the disk signature (new for each
# build) and the partition number, which the stick's own limine.conf names.
# Runs after ci/make-windows-img.sh and ci/build-image.sh, which leaves the
# files in out/iso. ntfsresize makes room on C:, ntfs-3g (FUSE: the container
# runs privileged) copies the host in; ntfscp cannot write a file that big.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${ARCTIC_OUT:-$ROOT/out}
ISO="$OUT/iso"
IMG="$OUT/arctic-usb.img"
WORK="$OUT/usb"
ESP_MB=500
C_FREE_MB=1024 # free on C: until the first start grows it
HOST_FILES=("$ISO/arctic/host.sqfs" "$ISO/arctic/vmlinuz" "$ISO/arctic/initrd.img")

if [ -z "${SKIP_DEPS:-}" ]; then
    source "$ROOT/ci/arch-prep.sh"
    pacman -Syu --noconfirm --needed dosfstools mtools limine util-linux ntfs-3g \
        sbsigntools binutils openssl libarchive
fi

[ -f "$ISO/arctic/host.sqfs" ] || { echo "run ci/build-image.sh first"; exit 1; }
[ -f "$OUT/windows.img" ] || { echo "run ci/make-windows-img.sh first"; exit 1; }
rm -rf "$WORK" "$IMG"
mkdir -p "$WORK/esp/EFI/Arctic" "$WORK/esp/boot/limine"

SIG=$(od -An -N4 -tx4 /dev/urandom | tr -d ' \n')
[ "$SIG" != 00000000 ] || SIG=a3c71c01
PARTUUID=$SIG-02

# Partition 1. Limine reads its config and the kernel from here on both
# UEFI and BIOS; the config, and so Limine, is the stick's own.
cp "$ISO/arctic/vmlinuz" "$ISO/arctic/initrd.img" "$WORK/esp/EFI/Arctic/"
cp /usr/share/limine/limine-bios.sys "$WORK/esp/boot/limine/"
bash "$ROOT/ci/limine-conf.sh" "$ROOT/image/limine-usb.conf" "$WORK/esp" "$WORK/esp/boot/limine/limine.conf" \
    PARTUUID="$PARTUUID"
bash "$ROOT/ci/sign-efi.sh" "$WORK/esp"
truncate -s "${ESP_MB}M" "$WORK/esp.img"
mkfs.fat -F 32 -h 2048 -n ARCTIC "$WORK/esp.img" >/dev/null
mcopy -s -Q -i "$WORK/esp.img" "$WORK/esp"/* ::

# Partition 2: windows.img brought to the size of what it holds, the host
# and C_FREE_MB, whole MiB
cp --sparse=always "$OUT/windows.img" "$WORK/c.img"
min=$(ntfsresize --info --force --no-progress-bar "$WORK/c.img" | awk '/You might resize at/ {print $5}')
[ -n "$min" ] || { echo "ntfsresize did not say how small C: can be"; exit 1; }
host=$(du -cb --apparent-size "${HOST_FILES[@]}" | tail -n1 | cut -f1)
size=$(( (min + host + C_FREE_MB * 1048576 + 1048575) / 1048576 * 1048576 ))
now=$(stat -c %s "$WORK/c.img")
[ "$size" -le "$now" ] || truncate -s "$size" "$WORK/c.img"
ntfsresize --force --no-progress-bar --size "$size" "$WORK/c.img" <<< y >/dev/null
[ "$size" -ge "$now" ] || truncate -s "$size" "$WORK/c.img"
# ntfsresize leaves the volume marked for a check; it was just made, it is clean
ntfsfix --clear-dirty "$WORK/c.img" >/dev/null
mkdir -p "$WORK/c"
ntfs-3g "$WORK/c.img" "$WORK/c"
mkdir -p "$WORK/c/Windows/System32/Host"
cp "${HOST_FILES[@]}" "$WORK/c/Windows/System32/Host/"
umount "$WORK/c"

# An NTFS boot sector, and its copy in the partition's last sector, record
# where the partition starts ("hidden sectors"); mkntfs wrote 0 for a file
le32() {
    printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $(($1 & 255)) $(($1 >> 8 & 255)) $(($1 >> 16 & 255)) $(($1 >> 24 & 255)))"
}
bps=$(od -An -tu2 -j 11 -N 2 "$WORK/c.img" | tr -d ' ')
[ "$bps" = 512 ] || { echo "C: has $bps-byte sectors; a stick has 512, Windows would not mount it"; exit 1; }
start=$(( (1 + ESP_MB) * 2048 ))
last=$(( $(stat -c %s "$WORK/c.img") / 512 - 1 )) # the partition's last sector, not the volume's
for sector in 0 "$last"; do
    [ "$(dd if="$WORK/c.img" bs=1 skip=$((sector * 512 + 3)) count=4 status=none | tr -d '\0')" = NTFS ] ||
        { echo "no NTFS boot sector at sector $sector of C:"; exit 1; }
    le32 "$start" | dd of="$WORK/c.img" bs=1 seek=$((sector * 512 + 28)) conv=notrunc status=none
done

# The disk
truncate -s $(( (1 + ESP_MB) * 1048576 + size )) "$IMG"
sfdisk --quiet "$IMG" <<SFDISK
label: dos
label-id: 0x$SIG
1MiB,${ESP_MB}MiB,ef,*
,,7
SFDISK
dd if="$WORK/esp.img" of="$IMG" bs=1M seek=1 conv=notrunc,sparse status=none
dd if="$WORK/c.img" of="$IMG" bs=1M seek=$((1 + ESP_MB)) conv=notrunc,sparse status=none
limine bios-install "$IMG"
[ "$(od -An -tx4 -j 440 -N 4 "$IMG" | tr -d ' \n')" = "$SIG" ] || { echo "the disk signature changed"; exit 1; }
rm -rf "$WORK"

echo "C: is PARTUUID=$PARTUUID, $((size / 1048576)) MB"
ls -l "$IMG"
