#!/bin/bash
# Packs host.sqfs, the initrd and the kernel into a BIOS + UEFI hybrid ISO.
# Runs in an archlinux container after build-rootfs.sh and make-windows-img.sh.
# UEFI boots through shim, so Secure Boot may stay on (ci/sign-efi.sh).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${ARCTIC_OUT:-$ROOT/out}
RFS="$OUT/rootfs"
ISO="$OUT/iso"

if [ -z "${SKIP_DEPS:-}" ]; then
    source "$ROOT/ci/arch-prep.sh"
    pacman -Syu --noconfirm --needed squashfs-tools xorriso limine cpio zstd \
        sbsigntools binutils openssl dosfstools mtools libarchive
fi

cp "$OUT/windows.img" "$RFS/usr/share/arctic/windows.img"

rm -rf "$ISO" "$OUT/arctic.iso"
mkdir -p "$ISO/boot/limine" "$ISO/EFI/BOOT" "$ISO/arctic"
mksquashfs "$RFS" "$ISO/arctic/host.sqfs" -comp zstd -Xcompression-level 15 -b 1M -noappend -quiet
(cd "$OUT/initrd" && find . | cpio -o -H newc --quiet) | zstd -19 -q > "$ISO/arctic/initrd.img"
cp "$OUT/kernel/vmlinuz" "$ISO/arctic/vmlinuz"

cp /usr/share/limine/limine-bios.sys /usr/share/limine/limine-bios-cd.bin "$ISO/boot/limine/"

# Every file Limine loads carries its BLAKE2b hash: with Secure Boot on,
# Limine loads nothing without one, and the config itself is enrolled
while IFS= read -r line; do
    if [[ $line =~ ^[[:space:]]*[a-z_]+:[[:space:]]*boot\(\):(/[^#[:space:]]+)$ ]]; then
        line="$line#$(b2sum "$ISO${BASH_REMATCH[1]}" | cut -d' ' -f1)"
    fi
    printf '%s\n' "$line"
done < "$ROOT/image/limine.conf" > "$ISO/boot/limine/limine.conf"
grep -q 'boot():/[^#]*#' "$ISO/boot/limine/limine.conf" || { echo "no hashes in limine.conf"; exit 1; }

bash "$ROOT/ci/sign-efi.sh" "$ISO"
cp "$OUT/efiboot.img" "$ISO/boot/efiboot.img"

xorriso -as mkisofs -iso-level 3 -V ARCTIC -R -r -J \
    -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
    -hfsplus -apm-block-size 2048 \
    --efi-boot boot/efiboot.img -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ISO" -o "$OUT/arctic.iso"
limine bios-install "$OUT/arctic.iso"

du -h "$ISO"/arctic/*
ls -l "$OUT/arctic.iso"
