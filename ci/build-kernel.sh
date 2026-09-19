#!/bin/bash
# Builds the Arctic kernel inside an archlinux container.
# Output: out/kernel/{vmlinuz,config,lib/modules/<release>/...}
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/ci/arch-prep.sh"
VER=$(cat "$ROOT/kernel/VERSION")
OUT="$ROOT/out/kernel"
WORK=/tmp/kbuild

pacman -Syu --noconfirm --needed base-devel bc cpio kmod libelf openssl pahole perl python tar xz zstd curl linux-headers

rm -rf "$OUT" "$WORK"
mkdir -p "$OUT" "$WORK"
cd "$WORK"
curl -fL --retry 5 -o linux.tar.xz "https://cdn.kernel.org/pub/linux/kernel/v${VER%%.*}.x/linux-$VER.tar.xz"
tar xf linux.tar.xz
cd "linux-$VER"

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

make -j"$(nproc)" bzImage modules
make INSTALL_MOD_PATH="$OUT" INSTALL_MOD_STRIP=1 modules_install
cp arch/x86/boot/bzImage "$OUT/vmlinuz"
cp .config "$OUT/config"
make -s kernelrelease > "$OUT/release"
rm -f "$OUT"/lib/modules/*/build "$OUT"/lib/modules/*/source

echo "kernel $(cat "$OUT/release") built"
du -sh "$OUT"/vmlinuz "$OUT"/lib/modules
