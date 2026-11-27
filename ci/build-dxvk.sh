#!/bin/bash
# Builds DXVK, Arctic's Direct3D 8-11 and DXGI: the upstream tag plus Arctic's
# patches (runtime/dxvk: H.264 video decoding on Vulkan Video), the DLLs for
# System32 (x64) and SysWOW64 (x86) as upstream's package-release.sh makes
# them. They stand in for the release package of runtime/manifest.txt.
#
# CI runs it in a clean archlinux container; tools/wsl/build-dxvk.sh runs it
# locally with a persistent work tree. A build with the tag, the patches and
# this script of the last one is skipped.
#
# Output: $DXVK_OUT (default out/dxvk)/{x64,x32}/*.dll
set -euo pipefail

ROOT=${ARCTIC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
TAG=$(cat "$ROOT/runtime/dxvk/VERSION")
AOUT=${ARCTIC_OUT:-$ROOT/out}
OUT=${DXVK_OUT:-$AOUT/dxvk}
WORK=${DXVK_WORK:-/tmp/dxvkbuild}
ARCHES=${DXVK_ARCHES:-"64 32"}

STAMP=$( { echo "$TAG $ARCHES"; cat "$ROOT"/runtime/dxvk/patches/*.patch "$ROOT/ci/build-dxvk.sh"; } | sha256sum | cut -d' ' -f1)
if [ "$(cat "$OUT/stamp" 2>/dev/null)" = "$STAMP" ] && [ -f "$OUT/x64/d3d11.dll" ]; then
    echo "dxvk $TAG: built as before, $OUT"
    exit 0
fi

if [ -z "${SKIP_DEPS:-}" ]; then
    source "$ROOT/ci/arch-prep.sh"
    pacman -Syu --noconfirm --needed git mingw-w64-gcc meson ninja glslang python
fi

# Pristine upstream with its submodules (Khronos headers, SPIR-V), fetched once per tag
if [ "$(cat "$WORK/upstream.tag" 2>/dev/null)" != "$TAG" ]; then
    rm -rf "$WORK/upstream"
    mkdir -p "$WORK"
    git clone --depth 1 --branch "$TAG" --recurse-submodules --shallow-submodules \
        https://github.com/doitsujin/dxvk.git "$WORK/upstream"
    echo "$TAG" > "$WORK/upstream.tag"
fi

# The work tree keeps its build directories: only files whose content
# changed get a new timestamp, so ninja rebuilds what the patches touch
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0='*'
rm -rf "$WORK/stage"
cp -a "$WORK/upstream" "$WORK/stage"
cd "$WORK/stage"
for patch in "$ROOT"/runtime/dxvk/patches/*.patch; do
    [ -e "$patch" ] || continue
    echo "applying $(basename "$patch")"
    git apply --whitespace=nowarn "$patch"
done
mkdir -p "$WORK/src"
rsync -rlc --delete --exclude=.git "$WORK/stage/" "$WORK/src/"
cd "$WORK/src"

for arch in $ARCHES; do
    cross=build-win64.txt
    [ "$arch" = 32 ] && cross=build-win32.txt
    if [ ! -f "$WORK/build.$arch/build.ninja" ]; then
        meson setup --cross-file "$cross" --buildtype release --strip -Dbuild_id=false \
            --prefix "$WORK/install.$arch" --bindir "x$arch" --libdir "x$arch" \
            "$WORK/build.$arch"
    fi
    ninja -C "$WORK/build.$arch" install
done

rm -rf "$OUT"
for arch in $ARCHES; do
    mkdir -p "$OUT/x$arch"
    cp "$WORK"/install.$arch/x$arch/*.dll "$OUT/x$arch/"
done
echo "$STAMP" > "$OUT/stamp"
ls -l "$OUT"/x*
