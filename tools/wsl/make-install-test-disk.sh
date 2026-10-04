#!/bin/bash
# A disk as an installation leaves it, for the VM (vm-test.py --boot-disk):
# GPT, an EFI system partition with \EFI\Arctic (and the same files in
# \EFI\BOOT, the path a VM's firmware boots without a boot entry), C: from
# the stick image with the given partition GUID, and a data partition after
# it, as D: stays next to Arctic on the hardware.
# usage: make-install-test-disk.sh <PARTUUID> <install set dir> <out.img>   (as root)
set -euo pipefail

PARTUUID=${1:?}; SET=${2:?}; IMG=${3:?}
USB=/root/arctic/out/arctic-usb.img
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

rm -f "$IMG"
truncate -s 8G "$IMG"
sfdisk --quiet "$IMG" <<EOF
label: gpt
start=2048, size=200MiB, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B
size=6GiB, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, uuid=$PARTUUID
type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
EOF
sfdisk -d "$IMG"

# the EFI system partition
truncate -s 200M "$W/esp"
mkfs.fat -F 32 -n SYSTEM "$W/esp" >/dev/null
mmd -i "$W/esp" ::EFI ::EFI/Arctic ::EFI/BOOT
mcopy -i "$W/esp" "$SET"/EFI/Arctic/* ::EFI/Arctic/
mcopy -i "$W/esp" "$SET/EFI/Arctic/shimx64.efi" ::EFI/BOOT/BOOTX64.EFI
mcopy -i "$W/esp" "$SET"/EFI/Arctic/{grubx64.efi,mmx64.efi,limine.conf,vmlinuz,initrd.img} ::EFI/BOOT/
dd if="$W/esp" of="$IMG" bs=1M seek=1 conv=notrunc,sparse status=none

# C: from the stick image's second partition, onto this disk's second one
read -r start size < <(sfdisk -d "$USB" | awk '/img2/ {gsub(",","",$4); gsub(",","",$6); print $4, $6}')
dest=$(sfdisk -d "$IMG" | awk -F'[=,]' '/img2/ {gsub(" ","",$2); print $2}')
dd if="$USB" of="$IMG" bs=4M iflag=skip_bytes,count_bytes oflag=seek_bytes skip=$((start * 512)) \
    count=$((size * 512)) seek=$((dest * 512)) conv=notrunc,sparse status=none

# "D:", an NTFS volume of its own
off=$(sfdisk -d "$IMG" | awk -F'[=,]' '/img3/ {gsub(" ","",$2); print $2}')
L=$(losetup -f --show -o $((off * 512)) "$IMG")
mkntfs -Q -q -L DATA "$L"
losetup -d "$L"
ls -l "$IMG"
