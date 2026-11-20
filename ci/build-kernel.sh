#!/bin/bash
# Builds the Arctic kernel inside an archlinux container.
# Output: out/kernel/{vmlinuz,config,lib/modules/<release>/...}
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=$(cat "$ROOT/kernel/VERSION")
OUT=${KERNEL_OUT:-$ROOT/out/kernel}
WORK=${KERNEL_WORK:-/tmp/kbuild}
PACKAGES="base-devel bc cpio kmod libelf openssl pahole perl python tar xz zstd curl linux-headers ccache
    python-pillow noto-fonts"

if [ -z "${SKIP_DEPS:-}" ]; then
    source "$ROOT/ci/arch-prep.sh"
    pacman -Syu --noconfirm --needed $PACKAGES
fi

# the sources are fetched once per version where they are kept (KERNEL_DOWNLOADS, locally)
DL=${KERNEL_DOWNLOADS:-$WORK}
# KERNEL_KEEP_TREE (locally, tools/wsl/build-kernel.sh): the built tree of the
# last run is kept while the version, the patches and the config are the same,
# so make compiles what changed (Arctic's drivers) and nothing else; a fresh
# tree goes through every object again, half an hour even when all of them
# come from ccache
STAMP=$(cat "$ROOT/kernel/VERSION" "$ROOT"/kernel/patches/*.patch "$ROOT/kernel/arctic.config" \
        "$ROOT/ci/mkpanic.py" $(ls /usr/lib/modules/*/build/.config | head -n1) | sha256sum | cut -d' ' -f1)
mkdir -p "$DL"
if [ -n "${KERNEL_KEEP_TREE:-}" ] && [ "$(cat "$WORK/stamp" 2>/dev/null)" = "$STAMP" ] &&
   [ -f "$WORK/linux-$VER/arch/x86/boot/bzImage" ]; then
    echo "the tree of the last build: only what changed is built"
    cd "$WORK/linux-$VER"
    rm -rf "$WORK/nvidia-"* "$WORK/legacy"
    FRESH=
else
rm -rf "$OUT" "$WORK"
mkdir -p "$WORK"
cd "$WORK"
[ -s "$DL/linux-$VER.tar.xz" ] ||
    curl -fL --retry 5 -o "$DL/linux-$VER.tar.xz" "https://cdn.kernel.org/pub/linux/kernel/v${VER%%.*}.x/linux-$VER.tar.xz"
tar xf "$DL/linux-$VER.tar.xz"
cd "linux-$VER"

# Arctic patches (kernel/patches) and the generated stop screen text for drm_panic
for patch in "$ROOT"/kernel/patches/*.patch; do
    [ -e "$patch" ] || continue
    echo "applying $(basename "$patch")"
    patch -p1 --no-backup-if-mismatch < "$patch"
done
python "$ROOT/ci/mkpanic.py" /usr/share/fonts/noto/NotoSans-Light.ttf drivers/gpu/drm/drm_panic_arctic.h
FRESH=1
fi

# Arctic's own drivers (kernel/drivers/<name>, built in: obj-y, no options of
# their own, so that a change to one rebuilds that one only), each with its
# uapi header as uapi-<name>.h
for dir in "$ROOT"/kernel/drivers/*/; do
    [ -d "$dir" ] || continue
    name=$(basename "$dir")
    echo "driver $name"
    rm -rf "drivers/$name"
    cp -r "$dir" "drivers/$name"
    if [ -f "drivers/$name/uapi-$name.h" ]; then
        # the same content keeps the old date: nothing that includes it is built again
        cmp -s "drivers/$name/uapi-$name.h" "include/uapi/linux/$name.h" ||
            cp "drivers/$name/uapi-$name.h" "include/uapi/linux/$name.h"
        rm "drivers/$name/uapi-$name.h"
    fi
    grep -qx "obj-y += $name/" drivers/Makefile || echo "obj-y += $name/" >> drivers/Makefile
done

if [ -n "$FRESH" ]; then
# Arch's own config ships in linux-headers
ARCH_CONFIG=$(ls /usr/lib/modules/*/build/.config | head -n1)
echo "base config: $ARCH_CONFIG"
cp "$ARCH_CONFIG" .config
# merge_config.sh does not take a path with spaces (a checkout in "project arctic")
cp "$ROOT/kernel/arctic.config" "$WORK/arctic.config"
scripts/kconfig/merge_config.sh -m .config "$WORK/arctic.config"
make olddefconfig
fi

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
# tools/wsl/build-kernel.sh): a fresh tree is always unpacked at the same
# path, so whatever did not change comes from the cache
source "$ROOT/ci/ccache-env.sh"
CC=(CC="ccache gcc" HOSTCC="ccache gcc")
echo "building with $(nproc) CPUs"
make -j"$(nproc)" "${CC[@]}" bzImage modules
echo "$STAMP" > "$WORK/stamp"  # this tree is built: KERNEL_KEEP_TREE may take it
ccache -s

# the kept tree with no module built since the last output (a change to a
# built-in driver such as win32k): the modules there, NVIDIA's too, are these
# very ones; only the kernel image is new. Installing ~6000 modules again
# (strip, sign, compress) and building NVIDIA's takes ten minutes.
if [ -z "$FRESH" ] && [ -f "$OUT/release" ] && [ -d "$OUT/nvidia-legacy" ] &&
   [ "$(cat "$OUT/release")" = "$(make -s kernelrelease)" ] &&
   [ -z "$(find . -name '*.ko' -newer "$OUT/release" -print -quit)" ]; then
    cp arch/x86/boot/bzImage "$OUT/vmlinuz"
    cp .config "$OUT/config"
    touch "$OUT/release"
    echo "kernel $(cat "$OUT/release") built (its modules as they were)"
    du -sh "$OUT"/vmlinuz
    exit 0
fi
rm -rf "$OUT"
mkdir -p "$OUT"
make INSTALL_MOD_PATH="$OUT" INSTALL_MOD_STRIP=1 modules_install

# NVIDIA's kernel modules for this very kernel, signed with its key like the
# modules above, both from the driver's .run of one version (kernel/
# NVIDIA_VERSION, the 580 branch: the last that drives Pascal and older):
# the open modules take Turing and newer and are what modprobe finds; the
# proprietary ones (Maxwell, Pascal, Volta) wait in nvidia-legacy/, outside
# the module tree, for arctic-gpu to load when the open module declines the
# card. build-rootfs.sh installs the user space from the same .run.
NV=$(cat "$ROOT/kernel/NVIDIA_VERSION")
[ -s "$DL/NVIDIA-Linux-x86_64-$NV.run" ] || curl -fL --retry 5 -o "$DL/NVIDIA-Linux-x86_64-$NV.run" \
    "https://download.nvidia.com/XFree86/Linux-x86_64/$NV/NVIDIA-Linux-x86_64-$NV.run"
sh "$DL/NVIDIA-Linux-x86_64-$NV.run" -x --target "$WORK/nvidia-$NV" > /dev/null
make -C "$WORK/nvidia-$NV/kernel-open" -j"$(nproc)" "${CC[@]}" SYSSRC="$PWD" SYSOUT="$PWD" modules
make -C "$WORK/nvidia-$NV/kernel-open" SYSSRC="$PWD" SYSOUT="$PWD" \
    INSTALL_MOD_PATH="$OUT" INSTALL_MOD_STRIP=1 modules_install
make -C "$WORK/nvidia-$NV/kernel" -j"$(nproc)" "${CC[@]}" SYSSRC="$PWD" SYSOUT="$PWD" modules
make -C "$WORK/nvidia-$NV/kernel" SYSSRC="$PWD" SYSOUT="$PWD" \
    INSTALL_MOD_PATH="$WORK/legacy" INSTALL_MOD_DIR=nvidia-legacy INSTALL_MOD_STRIP=1 modules_install
mkdir -p "$OUT/nvidia-legacy"
find "$WORK/legacy" -path '*/nvidia-legacy/*.ko*' -exec cp {} "$OUT/nvidia-legacy/" \;
ls "$OUT"/nvidia-legacy/
depmod -b "$OUT" "$(make -s kernelrelease)"
ls "$OUT"/lib/modules/*/kernel/drivers/video/nvidia*.ko*

cp arch/x86/boot/bzImage "$OUT/vmlinuz"
cp .config "$OUT/config"
make -s kernelrelease > "$OUT/release"
rm -f "$OUT"/lib/modules/*/build "$OUT"/lib/modules/*/source

echo "kernel $(cat "$OUT/release") built"
du -sh "$OUT"/vmlinuz "$OUT"/lib/modules
