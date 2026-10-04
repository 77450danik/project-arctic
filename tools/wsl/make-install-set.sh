#!/bin/bash
# The boot files of Arctic installed on an internal disk, for \EFI\Arctic on
# the EFI system partition next to Windows Boot Manager: shim (shimx64.efi,
# which the firmware entry "ARCTIC" starts), Limine signed with this
# install's own config, MokManager, the kernel and the initrd of the last
# image build. C: is named by its GPT partition GUID.
# usage: make-install-set.sh <PARTUUID of C:> <out dir>   (as root in arctic-build)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PARTUUID=${1:?usage: make-install-set.sh <PARTUUID> <out dir>}
DEST=${2:?usage: make-install-set.sh <PARTUUID> <out dir>}
OUT=/root/arctic/out
export ARCTIC_OUT=$OUT SKIP_DEPS=1

TREE=$(mktemp -d)
trap 'rm -rf "$TREE"' EXIT
mkdir -p "$TREE/EFI/Arctic" "$TREE/boot/limine"
cp "$OUT/iso/arctic/vmlinuz" "$OUT/iso/arctic/initrd.img" "$TREE/EFI/Arctic/"
bash "$ROOT/ci/limine-conf.sh" "$ROOT/image/limine-disk.conf" "$TREE" "$TREE/boot/limine/limine.conf" \
    PARTUUID="${PARTUUID,,}"
bash "$ROOT/ci/sign-efi.sh" "$TREE" # puts shim, Limine and MokManager in EFI/BOOT

rm -rf "$DEST"
mkdir -p "$DEST/EFI/Arctic"
cp "$TREE/EFI/BOOT/BOOTX64.EFI" "$DEST/EFI/Arctic/shimx64.efi"
cp "$TREE/EFI/BOOT/grubx64.efi" "$TREE/EFI/BOOT/mmx64.efi" "$DEST/EFI/Arctic/"
cp "$TREE/EFI/Arctic/vmlinuz" "$TREE/EFI/Arctic/initrd.img" "$DEST/EFI/Arctic/"
cp "$TREE/boot/limine/limine.conf" "$DEST/EFI/Arctic/limine.conf" # Limine looks next to itself first
cp "$TREE/ARCTIC.cer" "$DEST/EFI/Arctic/"
ls -l "$DEST/EFI/Arctic"
grep -n "PARTUUID" "$DEST/EFI/Arctic/limine.conf"
