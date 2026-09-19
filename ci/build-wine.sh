#!/bin/bash
# Builds Wine inside an archlinux container (new WoW64: 64-bit unix side only).
# Output: out/wine/usr/{bin,lib/wine,share/wine}
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/ci/arch-prep.sh"
TAG=$(cat "$ROOT/runtime/WINE_VERSION")
OUT="$ROOT/out/wine"
WORK=/tmp/winebuild

pacman -Syu --noconfirm --needed base-devel git mingw-w64-gcc \
    freetype2 gnutls alsa-lib vulkan-icd-loader vulkan-headers systemd-libs libusb \
    wayland libxkbcommon

rm -rf "$OUT" "$WORK"
mkdir -p "$OUT" "$WORK/build"
git clone --depth 1 --branch "$TAG" https://github.com/wine-mirror/wine.git "$WORK/src"

# Arctic modules and patches on top of upstream (docs/M1-display.md)
cd "$WORK/src"
cp -a "$ROOT/runtime/wine/modules/." . && rm -f .keep
for patch in "$ROOT"/runtime/wine/patches/*.patch; do
    [ -e "$patch" ] || continue
    echo "applying $(basename "$patch")"
    git apply --whitespace=nowarn "$patch"
done
perl tools/make_requests
if [ -n "$(find "$ROOT/runtime/wine/modules" -mindepth 1 ! -name .keep -print -quit)" ]; then
    perl tools/make_makefiles
    autoreconf -fi
fi

cd "$WORK/build"
../src/configure \
    --prefix=/usr --libdir=/usr/lib \
    --enable-archs=i386,x86_64 \
    --with-alsa --with-freetype --with-gnutls --with-udev --with-usb \
    --with-vulkan --with-wayland \
    --without-x --without-opengl --without-fontconfig --without-dbus \
    --without-cups --without-pulse --without-oss --without-sdl \
    --without-gphoto --without-sane --without-v4l2 --without-pcap \
    --without-netapi --without-gssapi --without-krb5 --without-capi \
    --without-opencl --without-pcsclite --without-gstreamer --without-ffmpeg \
    CFLAGS="-O2 -pipe" CROSSCFLAGS="-O2 -pipe"

make -j"$(nproc)"
make install-lib DESTDIR="$OUT"
# for compile checks of runtime changes on the build machine
cp include/config.h "$OUT/build-config.h"

find "$OUT/usr/lib/wine" -name '*.so' -exec strip --strip-unneeded {} +
find "$OUT/usr/bin" -type f -exec sh -c 'file -b "$1" | grep -q ELF && strip --strip-unneeded "$1"' _ {} \;
rm -rf "$OUT/usr/share/man" "$OUT/usr/share/applications"

echo "$TAG" > "$OUT/version"
du -sh "$OUT"/usr/lib/wine/* "$OUT"/usr/share/wine
