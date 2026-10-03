#!/bin/bash
# Limine's x86-64 UEFI loader with Arctic's patches (runtime/limine), at the
# version of the limine package installed, whose host tool enrolls the
# config hash into it (sign-efi.sh). Arch's own build stops at a prompt on
# laptops whose firmware refuses TPM measurements.
# usage: build-limine.sh <out dir>  -> <out dir>/BOOTX64.EFI
# A build is kept in LIMINE_CACHE (default ~/.cache/arctic-limine) by
# version and patches, so it is made once.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST=${1:?usage: build-limine.sh <out dir>}
VER=$(limine --version | awk '{print $NF; exit}')
PATCHES=$(cat "$ROOT"/runtime/limine/*.patch | sha256sum | cut -c1-16)
CACHE=${LIMINE_CACHE:-$HOME/.cache/arctic-limine}/$VER-$PATCHES
mkdir -p "$DEST"

if [ ! -f "$CACHE/BOOTX64.EFI" ]; then
    command -v clang >/dev/null && command -v ld.lld >/dev/null && command -v nasm >/dev/null ||
        pacman -S --noconfirm --needed clang lld llvm nasm >/dev/null
    WORK=$(mktemp -d)
    trap 'rm -rf "$WORK"' EXIT
    curl -fsSL --retry 5 -o "$WORK/limine.tar.gz" \
        "https://github.com/limine-bootloader/limine/releases/download/v$VER/limine-$VER.tar.gz" ||
        curl -fsSL --retry 5 -o "$WORK/limine.tar.gz" \
            "https://codeberg.org/Limine/Limine/releases/download/v$VER/limine-$VER.tar.gz"
    tar -xzf "$WORK/limine.tar.gz" -C "$WORK"
    SRC="$WORK/limine-$VER"
    for patch in "$ROOT"/runtime/limine/*.patch; do
        echo "limine: applying $(basename "$patch")"
        patch -d "$SRC" -p1 --quiet < "$patch"
    done
    (cd "$SRC" && ./configure --enable-uefi-x86-64 >/dev/null && make -j"$(nproc)" >/dev/null)
    mkdir -p "$CACHE"
    cp "$SRC/bin/BOOTX64.EFI" "$CACHE/BOOTX64.EFI"
fi
cp "$CACHE/BOOTX64.EFI" "$DEST/BOOTX64.EFI"
echo "limine $VER with Arctic's patches: $DEST/BOOTX64.EFI"
