#!/bin/bash
# Builds Wine: the upstream tag plus Arctic modules and patches, new WoW64
# (64-bit unix side only).
#
# CI runs it once in a clean archlinux container. tools/wsl/build-wine.sh runs
# it locally with a persistent WINE_WORK, and then only what changed gets
# rebuilt: files are synced into the build tree by content, so an untouched
# file keeps its timestamp even when it was regenerated.
#
# Output: $WINE_OUT (default out/wine)/usr/{bin,lib/wine,share/wine}
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TAG=$(cat "$ROOT/runtime/WINE_VERSION")
OUT=${WINE_OUT:-$ROOT/out/wine}
WORK=${WINE_WORK:-/tmp/winebuild}

if [ -z "${SKIP_DEPS:-}" ]; then
    source "$ROOT/ci/arch-prep.sh"
    pacman -Syu --noconfirm --needed base-devel git rsync mingw-w64-gcc \
        freetype2 gnutls alsa-lib vulkan-icd-loader vulkan-headers systemd-libs libusb \
        wayland libxkbcommon
fi

# Pristine upstream, fetched once per tag
if [ "$(cat "$WORK/upstream.tag" 2>/dev/null)" != "$TAG" ]; then
    rm -rf "$WORK"
    mkdir -p "$WORK"
    git clone --depth 1 --branch "$TAG" https://github.com/wine-mirror/wine.git "$WORK/upstream"
    echo "$TAG" > "$WORK/upstream.tag"
fi

# Arctic modules and patches on top of upstream (docs/M1-display.md)
rm -rf "$WORK/stage"
cp -a "$WORK/upstream" "$WORK/stage"
cd "$WORK/stage"
cp -a "$ROOT/runtime/wine/modules/." . && rm -f .keep
for patch in "$ROOT"/runtime/wine/patches/*.patch; do
    [ -e "$patch" ] || continue
    echo "applying $(basename "$patch")"
    git apply --whitespace=nowarn "$patch"
done
perl tools/make_requests
if [ -n "$(find "$ROOT/runtime/wine/modules" -mindepth 1 ! -name .keep -print -quit)" ]; then
    git add -A . # make_makefiles only sees files git knows about
    perl tools/make_makefiles
    autoreconf -fi
fi

# Only files whose content changed get a new timestamp in the build tree
mkdir -p "$WORK/src" "$WORK/build"
rsync -rlc --delete --exclude=.git --exclude=autom4te.cache "$WORK/stage/" "$WORK/src/"

cd "$WORK/build"
if [ ! -f Makefile ]; then
    cc=() # ccache when available: fast rebuilds after a reconfigure
    if command -v ccache >/dev/null; then
        cc=(CC="ccache gcc" x86_64_CC="ccache x86_64-w64-mingw32-gcc" i386_CC="ccache i686-w64-mingw32-gcc")
    fi
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
        CFLAGS="-O2 -pipe" CROSSCFLAGS="-O2 -pipe" "${cc[@]}"
fi

make -j"$(nproc)"
rm -rf "$OUT"
make install-lib DESTDIR="$OUT"
# for compile checks of runtime changes on the build machine
cp include/config.h "$OUT/build-config.h"

find "$OUT/usr/lib/wine" -name '*.so' -exec strip --strip-unneeded {} +
find "$OUT/usr/bin" -type f -exec sh -c 'file -b "$1" | grep -q ELF && strip --strip-unneeded "$1"' _ {} \;
rm -rf "$OUT/usr/share/man" "$OUT/usr/share/applications"

echo "$TAG" > "$OUT/version"
du -sh "$OUT"/usr/lib/wine/* "$OUT"/usr/share/wine
