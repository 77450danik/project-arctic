#!/bin/bash
# Assembles the host root filesystem and the NT prefix. Runs in a privileged
# archlinux container and needs out/kernel and out/wine.
# Output: out/rootfs, out/prefix (with drive_c), out/initrd
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/ci/arch-prep.sh"
OUT="$ROOT/out"
RFS="$OUT/rootfs"

pacman -Syu --noconfirm --needed arch-install-scripts gcc python-pillow noto-fonts

# The container's NoExtract rules drop the locale sources we need
sed '/^NoExtract/d' /etc/pacman.conf > /tmp/pacman.conf
rm -rf "$RFS" "$OUT/prefix" "$OUT/initrd"
mkdir -p "$RFS"
# shellcheck disable=SC2046
pacstrap -C /tmp/pacman.conf -c -G -M "$RFS" $(grep -v '^[[:space:]]*#' "$ROOT/host/rootfs/packages.txt")

chroot "$RFS" localedef -i uk_UA -f UTF-8 uk_UA.UTF-8
chroot "$RFS" localedef -i en_US -f UTF-8 en_US.UTF-8
systemd-sysusers --root="$RFS"
systemd-hwdb update --root="$RFS" --usr
useradd -R "$RFS" -u 1000 -U -d /run/nt -M -s /usr/bin/nologin -G video,render,input,audio nt

cp -a "$ROOT/host/rootfs/files/." "$RFS/"
ln -sf /usr/share/zoneinfo/Europe/Kyiv "$RFS/etc/localtime"
echo arctic > "$RFS/etc/hostname"
mkdir -p "$RFS/mnt/c" "$RFS/usr/share/arctic"

cp -a "$OUT/kernel/lib/modules" "$RFS/usr/lib/"
cp -a "$OUT/wine/usr/." "$RFS/usr/"

INIT_SRC=("$ROOT"/host/init/{splash,screen,stop}.c)
gcc -O2 -Wall -o "$RFS/usr/bin/arctic-init" "$ROOT/host/init/arctic-init.c" "${INIT_SRC[@]}"
mkdir -p "$OUT/initrd/dev" "$OUT/initrd/proc" "$OUT/initrd/sys"
gcc -static -Os -Wall -o "$OUT/initrd/init" "$ROOT/host/init/initrd-init.c" "${INIT_SRC[@]}"
mknod -m 600 "$OUT/initrd/dev/console" c 5 1
python "$ROOT/ci/logo2raw.py" "$ROOT/ARCTIC.png" "$OUT/initrd/logo.bgra"
cp "$OUT/initrd/logo.bgra" "$RFS/usr/share/arctic/logo.bgra"
python "$ROOT/ci/mkfont.py" /usr/share/fonts/noto/NotoSans-Light.ttf "$OUT/initrd/bsod.font"
cp "$OUT/initrd/bsod.font" "$RFS/usr/share/arctic/bsod.font"

# NT prefix, created by the Wine that ships in this very image
cat > "$RFS/var/tmp/mkprefix.sh" <<'EOF'
set -e
export WINEPREFIX=/var/tmp/prefix HOME=/var/tmp/home USER=User LOGNAME=User LANG=uk_UA.UTF-8
export WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
mkdir -p "$HOME"
wine wineboot.exe --init
wine winecfg.exe -v win11
wineserver -w
EOF
timeout 900 arch-chroot "$RFS" /usr/bin/busybox sh /var/tmp/mkprefix.sh

PFX="$RFS/var/tmp/prefix"
rm -f "$PFX/dosdevices/z:"
mkdir -p "$PFX/drive_c/windows/system32/config"
cp "$PFX/system.reg" "$PFX/user.reg" "$PFX/userdef.reg" "$PFX/.update-timestamp" "$PFX/drive_c/windows/system32/config/"
mv "$PFX" "$OUT/prefix"
rm -rf "$RFS/var/tmp/"*

# Nothing here is ever read on the running system
rm -rf "$RFS"/usr/share/{man,doc,info,gtk-doc,help,i18n,locale} "$RFS/usr/include" "$RFS"/var/cache/pacman/pkg/*
find "$RFS/usr/lib" -name '*.a' -delete

du -sh "$RFS" "$OUT/prefix/drive_c"
