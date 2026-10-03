#!/bin/bash
# Builds the Arctic kernel inside an archlinux container.
# Output: out/kernel/{vmlinuz,config,lib/modules/<release>/...}
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/ci/arch-prep.sh"
VER=$(cat "$ROOT/kernel/VERSION")
OUT="$ROOT/out/kernel"
WORK=/tmp/kbuild

pacman -Syu --noconfirm --needed base-devel bc cpio kmod libelf openssl pahole perl python tar xz zstd curl linux-headers ccache \
    python-pillow noto-fonts

rm -rf "$OUT" "$WORK"
mkdir -p "$OUT" "$WORK"
cd "$WORK"
curl -fL --retry 5 -o linux.tar.xz "https://cdn.kernel.org/pub/linux/kernel/v${VER%%.*}.x/linux-$VER.tar.xz"
tar xf linux.tar.xz
cd "linux-$VER"

# Arctic patches (kernel/patches) and the generated stop screen text for drm_panic
for patch in "$ROOT"/kernel/patches/*.patch; do
    [ -e "$patch" ] || continue
    echo "applying $(basename "$patch")"
    patch -p1 --no-backup-if-mismatch < "$patch"
done
python "$ROOT/ci/mkpanic.py" /usr/share/fonts/noto/NotoSans-Light.ttf drivers/gpu/drm/drm_panic_arctic.h

# Arch's own config ships in linux-headers
ARCH_CONFIG=$(ls /usr/lib/modules/*/build/.config | head -n1)
echo "base config: $ARCH_CONFIG"
cp "$ARCH_CONFIG" .config
scripts/kconfig/merge_config.sh -m .config "$ROOT/kernel/arctic.config"
make olddefconfig

# Fail loudly if a required option was dropped by a dependency
missing=0
while IFS= read -r line; do
    case "$line" in
        CONFIG_*=y) grep -qx "$line" .config || { echo "MISSING: $line"; missing=1; } ;;
        "# CONFIG_"*" is not set")
            opt=${line#"# "}; opt=${opt%" is not set"}
            ! grep -q "^$opt=[ym]" .config || { echo "STILL SET: $opt"; missing=1; } ;;
    esac
done < "$ROOT/kernel/arctic.config"
[ "$missing" = 0 ] || exit 1

# ccache of the last run (CCACHE_DIR, kept by the workflow; locally
# tools/wsl/build-kernel.sh): the tree is unpacked anew each time, always at
# the same path, so whatever did not change comes from the cache
source "$ROOT/ci/ccache-env.sh"
CC=(CC="ccache gcc" HOSTCC="ccache gcc")
echo "building with $(nproc) CPUs"
make -j"$(nproc)" "${CC[@]}" bzImage modules
ccache -s
make INSTALL_MOD_PATH="$OUT" INSTALL_MOD_STRIP=1 modules_install

# NVIDIA's open kernel modules (Turing and newer) for this very kernel, signed
# with its key like the modules above. Their user space must be the same
# version: build-rootfs.sh takes it from the Arch archive by this number.
NV=$(cat "$ROOT/kernel/NVIDIA_VERSION")
curl -fL --retry 5 -o "$WORK/nvidia.tar.gz" \
    "https://github.com/NVIDIA/open-gpu-kernel-modules/archive/refs/tags/$NV.tar.gz"
tar xf "$WORK/nvidia.tar.gz" -C "$WORK"
make -C "$WORK/open-gpu-kernel-modules-$NV" -j"$(nproc)" "${CC[@]}" SYSSRC="$PWD" SYSOUT="$PWD" modules
make -C "$WORK/open-gpu-kernel-modules-$NV" SYSSRC="$PWD" SYSOUT="$PWD" \
    INSTALL_MOD_PATH="$OUT" INSTALL_MOD_STRIP=1 modules_install
depmod -b "$OUT" "$(make -s kernelrelease)"
ls "$OUT"/lib/modules/*/kernel/drivers/video/nvidia*.ko*

cp arch/x86/boot/bzImage "$OUT/vmlinuz"
cp .config "$OUT/config"
make -s kernelrelease > "$OUT/release"
rm -f "$OUT"/lib/modules/*/build "$OUT"/lib/modules/*/source

echo "kernel $(cat "$OUT/release") built"
du -sh "$OUT"/vmlinuz "$OUT"/lib/modules
