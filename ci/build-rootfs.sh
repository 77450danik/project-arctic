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
    pacman -Syu --noconfirm --needed arch-install-scripts gcc python-pillow noto-fonts ttf-liberation python-fonttools 7zip
fi

# The container's NoExtract rules drop the locale sources we need
sed '/^NoExtract/d' /etc/pacman.conf > /tmp/pacman.conf
rm -rf "$RFS" "$OUT/prefix" "$OUT/initrd"
mkdir -p "$RFS"
# shellcheck disable=SC2046
pacstrap -C /tmp/pacman.conf -c -G -M "$RFS" $(grep -v '^[[:space:]]*#' "$ROOT/host/rootfs/packages.txt")
# NVIDIA's user space, the same version as its kernel modules (build-kernel.sh),
# from the same .run: Arch packages only the newest branch, and the 580 branch
# is the last that drives Pascal and older
NV=$(cat "$ROOT/kernel/NVIDIA_VERSION")
NVDL=${KERNEL_DOWNLOADS:-/tmp}
mkdir -p "$NVDL"
[ -s "$NVDL/NVIDIA-Linux-x86_64-$NV.run" ] || curl -fL --retry 5 -o "$NVDL/NVIDIA-Linux-x86_64-$NV.run" \
    "https://download.nvidia.com/XFree86/Linux-x86_64/$NV/NVIDIA-Linux-x86_64-$NV.run"
rm -rf /tmp/nvidia-run
sh "$NVDL/NVIDIA-Linux-x86_64-$NV.run" -x --target /tmp/nvidia-run > /dev/null
python3 "$ROOT/ci/install-nvidia-userspace.py" /tmp/nvidia-run "$RFS"
rm -rf /tmp/nvidia-run
ldconfig -r "$RFS"

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
# the proprietary NVIDIA modules for Maxwell, Pascal and Volta (arctic-gpu loads them)
if [ -d "$OUT/kernel/nvidia-legacy" ]; then
    mkdir -p "$RFS/usr/lib/arctic"
    cp -a "$OUT/kernel/nvidia-legacy" "$RFS/usr/lib/arctic/"
fi
cp -a "$OUT/wine/usr/." "$RFS/usr/"

INIT_SRC=("$ROOT"/host/init/{bootanim,screen,stop,hiberfil}.c)
gcc -O2 -Wall -o "$RFS/usr/bin/arctic-init" "$ROOT/host/init/arctic-init.c" "${INIT_SRC[@]}"
gcc -O2 -Wall -o "$RFS/usr/bin/arctic-volume" "$ROOT/host/init/arctic-volume.c"
gcc -O2 -Wall -o "$RFS/usr/bin/arctic-gpu" "$ROOT/host/init/arctic-gpu.c"
# WSL in Arctic: the distributions of D:\WSL, for wsl.exe and bash.exe (docs/updates.md)
gcc -O2 -Wall -o "$RFS/usr/bin/arctic-lxss" "$ROOT/host/init/arctic-lxss.c"
# Network folders: \\server\share mounted on demand (docs/M5-network.md)
gcc -O2 -Wall -o "$RFS/usr/bin/arctic-smb" "$ROOT/host/init/arctic-smb.c" -lpthread
# Networking: name servers of every interface in one resolv.conf on /run
# (iwd and the wired DHCP script call resolvconf), names resolved by glibc
gcc -O2 -Wall -o "$RFS/usr/bin/resolvconf" "$ROOT/host/init/arctic-resolv.c"
chmod 755 "$RFS/usr/lib/arctic/udhcpc.script"
ln -sf /run/arctic/resolv.conf "$RFS/etc/resolv.conf"
sed -i 's/^hosts:.*/hosts: files dns/' "$RFS/etc/nsswitch.conf"
# Sound: arctic-init picks the default card at boot and writes it on /run
ln -sf /run/arctic/asound.conf "$RFS/etc/asound.conf"
mkdir -p "$RFS/var/lib/iwd"
tr -d '-' < /proc/sys/kernel/random/uuid > "$RFS/etc/machine-id"
mkdir -p "$OUT/initrd/dev" "$OUT/initrd/proc" "$OUT/initrd/sys"
gcc -static -Os -Wall -o "$OUT/initrd/init" "$ROOT/host/init/initrd-init.c" "${INIT_SRC[@]}"
# checking C: after the stick was pulled out, growing it on the first start
bash "$ROOT/ci/build-ntfsprogs.sh" "$OUT/initrd/bin"
mknod -m 600 "$OUT/initrd/dev/console" c 5 1
python "$ROOT/ci/logo2raw.py" "$ROOT/ARCTIC.png" "$OUT/initrd/logo.bgra"
cp "$OUT/initrd/logo.bgra" "$RFS/usr/share/arctic/logo.bgra"
python "$ROOT/ci/mkfont.py" /usr/share/fonts/noto/NotoSans-Light.ttf "$OUT/initrd/bsod.font" \
    /usr/share/fonts/noto/NotoSans-Regular.ttf
# Windows 11's boot spinner, which the boot screen (bootanim.c) draws under the logo
python "$ROOT/ci/mkspinner.py" "$ROOT/runtime/art/boot/segoe_slboot_ex.ttf" "$OUT/initrd/spinner.bin"
# and arctic-init's "Завершення роботи" screen
cp "$OUT/initrd/spinner.bin" "$RFS/usr/share/arctic/spinner.bin"
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
wine reg.exe add 'HKCU\Control Panel\Desktop' /v WallpaperStyle /d 10 /f
for reg in /usr/share/arctic/registry/*.reg; do wine reg.exe import "Z:$reg"; done
wineserver -w
EOF
# Arctic's registry settings stay in the image: when an update brings a newer
# Wine, wineboot brings C:'s registry up to it from wine.inf, and arctic-init
# applies these again over it
mkdir -p "$RFS/usr/share/arctic/registry"
cp "$ROOT"/runtime/registry/*.reg "$RFS/usr/share/arctic/registry/"
# Arctic's version, which the About dialog (ShellAbout, winver) shows: the
# build is the number of commits, the date the day the image was built. CI
# passes them in (its container has no git); a local build counts them itself.
BUILD=${ARCTIC_BUILD:-$(git -c safe.directory='*' -C "$ROOT" rev-list --count HEAD 2>/dev/null || echo 0)}
COMMIT=${ARCTIC_COMMIT:-$(git -c safe.directory='*' -C "$ROOT" rev-parse --short=7 HEAD 2>/dev/null || echo unknown)}
BUILD_DATE=${ARCTIC_BUILD_DATE:-$(date -u +%Y-%m-%d)}
cat > "$RFS/usr/share/arctic/registry/zz-build.reg" <<EOF
REGEDIT4

[HKEY_LOCAL_MACHINE\\Software\\Arctic]
"Version"="pre-alpha"
"Build"="$BUILD"
"BuildDate"="$BUILD_DATE"
"Commit"="$COMMIT"
EOF
echo "Arctic pre-alpha, build $BUILD of $BUILD_DATE ($COMMIT)"
timeout 900 arch-chroot "$RFS" /usr/bin/busybox sh /var/tmp/mkprefix.sh

PFX="$RFS/var/tmp/prefix"
rm -f "$PFX/dosdevices/z:"
mkdir -p "$PFX/drive_c/windows/Web/Wallpaper/Arctic"
# Windows always has LocalLow; certificate revocation caches (cryptnet) and
# low-integrity programs such as browsers look it up and give up without it
for profile in "$PFX"/drive_c/users/*/AppData; do mkdir -p "$profile/LocalLow"; done
cp "$ROOT/WALLPAPER.jpg" "$PFX/drive_c/windows/Web/Wallpaper/Arctic/img0.jpg"
# Parts of ReactOS (ci/build-reactos.sh), when they are built
REACTOS=${REACTOS_OUT:-$ROOT/out/reactos}
if [ -d "$REACTOS/Windows" ]; then
    cp -a "$REACTOS/Windows/System32/." "$PFX/drive_c/windows/system32/"
    find "$REACTOS/Windows" -maxdepth 1 -type f -exec cp -a {} "$PFX/drive_c/windows/" \;
    # There is one explorer.exe, in C:\Windows, as in Windows: Wine's own
    # never runs, so it is not in the image at all. Same for the Display
    # control panel: Wine's desk.cpl is its virtual desktop settings, and the
    # loader would take it over the one in System32
    rm -f "$PFX/drive_c/windows/system32/explorer.exe" "$RFS"/usr/lib/wine/*-windows/explorer.exe
    rm -f "$RFS"/usr/lib/wine/*-windows/desk.cpl
    # into Wine's resources	hemes, which holds aero: C: shows one Resources folder
    if [ -d "$REACTOS/Windows/Media" ]; then
        mkdir -p "$PFX/drive_c/windows/Media"
        cp -a "$REACTOS/Windows/Media/." "$PFX/drive_c/windows/Media/"
    fi
    if [ -d "$REACTOS/Windows/Resources/Themes" ]; then
        mkdir -p "$PFX/drive_c/windows/resources/themes"
        cp -a "$REACTOS/Windows/Resources/Themes/." "$PFX/drive_c/windows/resources/themes/"
    fi
    # the font of the visual style
    cp "$ROOT"/runtime/fonts/*.ttf "$PFX/drive_c/windows/Fonts/"
    # Arial, Times New Roman and Courier New, made from Liberation: Chromium
    # draws no text without them (ci/windows-fonts.py)
    python3 "$ROOT/ci/windows-fonts.py" /usr/share/fonts/liberation "$PFX/drive_c/windows/Fonts"
    mkdir -p "$PFX/drive_c/windows/Licenses"
    cp /usr/share/licenses/ttf-liberation/LICENSE "$PFX/drive_c/windows/Licenses/Liberation Fonts.txt"
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
for dll in shell32 browseui shdocvw shlwapi comctl32 uxtheme zipfldr stobject netshell; do
    timeout 120 wine regsvr32.exe /s "$dll.dll" || echo "regsvr32 $dll: $?"
done
wineserver -w
EOF
    timeout 900 arch-chroot "$RFS" /usr/bin/busybox sh /var/tmp/register.sh
fi

# Direct3D 8-12 on Vulkan and NVAPI as System32 / SysWOW64 DLLs
# (runtime/manifest.txt), and NVIDIA's DLSS core from its driver
GFX=$(mktemp -d)
while read -r name url sum; do
    case "$name" in ''|'#'*) continue ;; esac
    curl -fL --retry 5 -o "$GFX/$name.pkg" "$url"
    echo "$sum  $GFX/$name.pkg" | sha256sum -c -
    mkdir -p "$GFX/$name"
    tar xf "$GFX/$name.pkg" -C "$GFX/$name"
    find "$GFX/$name" -path '*/x64/*.dll' -exec cp {} "$PFX/drive_c/windows/system32/" \;
    find "$GFX/$name" \( -path '*/x32/*.dll' -o -path '*/x86/*.dll' \) -exec cp {} "$PFX/drive_c/windows/syswow64/" \;
done < "$ROOT/runtime/manifest.txt"
rm -rf "$GFX"
ls -l "$PFX"/drive_c/windows/system32/{d3d9,d3d11,dxgi,d3d12,d3d12core,nvapi64}.dll
cp "$RFS"/usr/lib/nvidia/wine/*.dll "$PFX/drive_c/windows/system32/" 2>/dev/null || true

# Google Chrome, the browser of the system (ci/install-chrome.sh)
bash "$ROOT/ci/install-chrome.sh" "$PFX"

# the Snipping Tool in Start, as in Windows 11 (Win+Shift+S starts it too)
python3 "$ROOT/ci/mklnk.py" "$PFX/drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs/Ножиці.lnk" \
    'C:\windows\system32\snippingtool.exe'

mkdir -p "$PFX/drive_c/windows/system32/config"
cp "$PFX/system.reg" "$PFX/user.reg" "$PFX/userdef.reg" "$PFX/.update-timestamp" "$PFX/drive_c/windows/system32/config/"
mv "$PFX" "$OUT/prefix"
rm -rf "$RFS/var/tmp/"*

# NVIDIA's parts for X11, Vulkan SC, data-centre GPUs, CUDA debugging and daemons
rm -rf "$RFS"/usr/lib/nvidia/xorg "$RFS"/usr/lib/xorg "$RFS"/usr/share/vulkansc
rm -f "$RFS"/usr/lib/lib{nvidia-vksc-core,nvidia-imex,nvidia-fmdrv,nvidia-pkcs11,nvidia-pkcs11-openssl3,cudadebugger}.so* \
    "$RFS"/usr/bin/nvidia-{bug-report.sh,cuda-mps-control,cuda-mps-server,debugdump,persistenced,powerd,pcc,xconfig,sleep.sh}

# Nothing here is ever read on the running system
rm -rf "$RFS"/usr/share/{man,doc,info,gtk-doc,help,i18n,locale} "$RFS/usr/include" "$RFS"/var/cache/pacman/pkg/*
find "$RFS/usr/lib" -name '*.a' -delete

du -sh "$RFS" "$OUT/prefix/drive_c"
