#!/bin/bash
# Assembles the host root filesystem and the NT prefix. Runs in a privileged
# archlinux container and needs out/kernel and out/wine.
# Output: out/rootfs, out/prefix (with drive_c), out/initrd
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${ARCTIC_OUT:-$ROOT/out}
RFS="$OUT/rootfs"

if [ -z "${SKIP_DEPS:-}" ]; then
    source "$ROOT/ci/arch-prep.sh"
    pacman -Syu --noconfirm --needed arch-install-scripts gcc python-pillow noto-fonts
fi

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
wine reg.exe add 'HKCU\Control Panel\Desktop' /v Wallpaper /d 'C:\Windows\Web\Wallpaper\Arctic\img0.jpg' /f
for reg in /var/tmp/registry/*.reg; do wine reg.exe import "Z:$reg"; done
wineserver -w
EOF
mkdir -p "$RFS/var/tmp/registry"
cp "$ROOT"/runtime/registry/*.reg "$RFS/var/tmp/registry/"
timeout 900 arch-chroot "$RFS" /usr/bin/busybox sh /var/tmp/mkprefix.sh

PFX="$RFS/var/tmp/prefix"
rm -f "$PFX/dosdevices/z:"
mkdir -p "$PFX/drive_c/windows/Web/Wallpaper/Arctic"
cp "$ROOT/WALLPAPER.jpg" "$PFX/drive_c/windows/Web/Wallpaper/Arctic/img0.jpg"
# Parts of ReactOS (ci/build-reactos.sh), when they are built
REACTOS=${REACTOS_OUT:-$ROOT/out/reactos}
if [ -d "$REACTOS/Windows" ]; then
    cp -a "$REACTOS/Windows/System32/." "$PFX/drive_c/windows/system32/"
    find "$REACTOS/Windows" -maxdepth 1 -type f -exec cp -a {} "$PFX/drive_c/windows/" \;
    # There is one explorer.exe, in C:\Windows, as in Windows: Wine's own
    # never runs, so it is not in the image at all
    rm -f "$PFX/drive_c/windows/system32/explorer.exe" "$RFS"/usr/lib/wine/*-windows/explorer.exe
    [ -d "$REACTOS/Windows/Resources" ] && cp -a "$REACTOS/Windows/Resources" "$PFX/drive_c/windows/"
    # The shell's manifest asks for common controls 6.0, so what it loads is the
    # side-by-side copy, not the one in system32
    for sxs in "$PFX"/drive_c/windows/winsxs/amd64_microsoft.windows.common-controls_*/comctl32.dll; do
        [ -e "$sxs" ] && [ -e "$REACTOS/Windows/System32/comctl32.dll" ] &&
            cp "$REACTOS/Windows/System32/comctl32.dll" "$sxs"
    done
fi
# The classes of the ReactOS shell, registered by the DLLs themselves
if [ -d "$REACTOS/Windows" ]; then
    cat > "$RFS/var/tmp/register.sh" <<'EOF'
set -e
# in C: ReactOS's DllRegisterServer fails under uk_UA (to be looked into);
# what it writes does not depend on the locale
export WINEPREFIX=/var/tmp/prefix HOME=/var/tmp/home USER=User LOGNAME=User LANG=C
export WINEDEBUG=-all
for dll in shell32 browseui shdocvw shlwapi comctl32 uxtheme; do
    timeout 120 wine regsvr32.exe /s "$dll.dll" || echo "regsvr32 $dll: $?"
done
wineserver -w
EOF
    timeout 900 arch-chroot "$RFS" /usr/bin/busybox sh /var/tmp/register.sh
fi

mkdir -p "$PFX/drive_c/windows/system32/config"
cp "$PFX/system.reg" "$PFX/user.reg" "$PFX/userdef.reg" "$PFX/.update-timestamp" "$PFX/drive_c/windows/system32/config/"
mv "$PFX" "$OUT/prefix"
rm -rf "$RFS/var/tmp/"*

# Nothing here is ever read on the running system
rm -rf "$RFS"/usr/share/{man,doc,info,gtk-doc,help,i18n,locale} "$RFS/usr/include" "$RFS"/var/cache/pacman/pkg/*
find "$RFS/usr/lib" -name '*.a' -delete

du -sh "$RFS" "$OUT/prefix/drive_c"
