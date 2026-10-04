#!/bin/bash
# ntfsck and ntfsresize from ntfsprogs-plus, the user-space tools of the
# kernel's ntfs driver, linked statically for the initrd: it checks C: when
# the stick was pulled out (the "dirty" flag) and grows C: over the whole
# stick on the first start. ntfs-3g's ntfsfix only clears the flag.
# usage: build-ntfsprogs.sh <out dir>  -> <out dir>/ntfsck, <out dir>/ntfsresize
# A build is kept in NTFSPROGS_CACHE (default ~/.cache/arctic-ntfsprogs).
set -euo pipefail

DEST=${1:?usage: build-ntfsprogs.sh <out dir>}
VER=1.0.0
SHA256=3e3d9d2f3620cdebb128fb8dbc116f2c2a035f18000e3f01af204694ef8c7778
CACHE=${NTFSPROGS_CACHE:-$HOME/.cache/arctic-ntfsprogs}/$VER
mkdir -p "$DEST"

if [ ! -x "$CACHE/ntfsck" ] || [ ! -x "$CACHE/ntfsresize" ]; then
    # make too: the CI container has none, and configure needs it for its
    # dependency fragments ("Something went wrong bootstrapping makefile fragments")
    command -v autoreconf >/dev/null && command -v libtoolize >/dev/null && command -v make >/dev/null ||
        pacman -S --noconfirm --needed autoconf automake libtool pkgconf make >/dev/null
    WORK=$(mktemp -d)
    trap 'rm -rf "$WORK"' EXIT
    curl -fsSL --retry 5 -o "$WORK/src.tar.gz" \
        "https://github.com/ntfsprogs-plus/ntfsprogs-plus/archive/refs/tags/$VER.tar.gz"
    echo "$SHA256  $WORK/src.tar.gz" | sha256sum -c --quiet
    tar -xzf "$WORK/src.tar.gz" -C "$WORK"
    SRC="$WORK/ntfsprogs-plus-$VER"
    (
        cd "$SRC"
        ./autogen.sh >/dev/null 2>&1
        ./configure --disable-shared --enable-static >/dev/null
        make -j"$(nproc)" -C libntfs >/dev/null
        # mkntfs and the rest are not needed (mkntfs wants a static libuuid)
        make -j"$(nproc)" -C src ntfsck ntfsresize LDFLAGS=-all-static >/dev/null 2>&1
    )
    mkdir -p "$CACHE"
    cp "$SRC/src/ntfsck" "$SRC/src/ntfsresize" "$CACHE/"
    strip "$CACHE/ntfsck" "$CACHE/ntfsresize"
fi
cp "$CACHE/ntfsck" "$CACHE/ntfsresize" "$DEST/"
echo "ntfsprogs-plus $VER: $DEST/ntfsck, $DEST/ntfsresize"
