#!/bin/bash
# Builds vkd3d-proton, Arctic's Direct3D 12: the upstream tag plus Arctic's
# patches (runtime/vkd3d-proton), d3d12.dll and d3d12core.dll for System32
# (x64) and SysWOW64 (x86), the way upstream's package-release.sh makes them.
#
# CI runs it in a clean archlinux container after the Wine job, whose widl it
# takes; tools/wsl/build-vkd3d-proton.sh runs it locally with a persistent
# work tree. A build with the tag, the patches and this script of the last
# one is skipped.
#
# Output: $VKD3D_OUT (default out/vkd3d-proton)/{x64,x86}/*.dll
set -euo pipefail

ROOT=${ARCTIC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
TAG=$(cat "$ROOT/runtime/vkd3d-proton/VERSION")
AOUT=${ARCTIC_OUT:-$ROOT/out}
OUT=${VKD3D_OUT:-$AOUT/vkd3d-proton}
WORK=${VKD3D_WORK:-/tmp/vkd3dbuild}

STAMP=$( { echo "$TAG"; cat "$ROOT"/runtime/vkd3d-proton/patches/*.patch "$ROOT/ci/build-vkd3d-proton.sh"; } | sha256sum | cut -d' ' -f1)
if [ "$(cat "$OUT/stamp" 2>/dev/null)" = "$STAMP" ] && [ -f "$OUT/x64/d3d12core.dll" ] && [ -f "$OUT/x86/d3d12core.dll" ]; then
    echo "vkd3d-proton $TAG: built as before, $OUT"
    exit 0
fi

if [ -z "${SKIP_DEPS:-}" ]; then
    source "$ROOT/ci/arch-prep.sh"
    pacman -Syu --noconfirm --needed git mingw-w64-gcc meson ninja glslang python
fi

# widl, the IDL compiler: Wine's, from its build (out/wine/build-tools in CI,
# the build tree on a build machine)
WIDL=${WIDL:-}
for w in "$WIDL" "$AOUT/wine/build-tools/widl" "${WINE_WORK:-/tmp/winebuild}/build/tools/widl/widl" "$(command -v widl || true)"; do
    if [ -n "$w" ] && [ -x "$w" ]; then WIDL=$w; break; fi
done
[ -n "$WIDL" ] && [ -x "$WIDL" ] || { echo "no widl: build Wine first (ci/build-wine.sh)"; exit 1; }
mkdir -p "$WORK/bin"
ln -sf "$WIDL" "$WORK/bin/widl"
export PATH="$WORK/bin:$PATH"

# Pristine upstream with its submodules (dxil-spirv, Khronos headers), fetched once per tag
if [ "$(cat "$WORK/upstream.tag" 2>/dev/null)" != "$TAG" ]; then
    rm -rf "$WORK/upstream"
    git clone --depth 1 --branch "$TAG" --recurse-submodules --shallow-submodules \
        https://github.com/HansKristian-Work/vkd3d-proton.git "$WORK/upstream"
    echo "$TAG" > "$WORK/upstream.tag"
fi

export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0='*'
rm -rf "$WORK/src"
cp -a "$WORK/upstream" "$WORK/src"
cd "$WORK/src"
for patch in "$ROOT"/runtime/vkd3d-proton/patches/*.patch; do
    [ -e "$patch" ] || continue
    echo "applying $(basename "$patch")"
    git apply --whitespace=nowarn "$patch"
done

for arch in 64 86; do
    cross=build-win64.txt
    [ "$arch" = 86 ] && cross=build-win32.txt
    rm -rf "$WORK/build.$arch" "$WORK/install.$arch"
    meson setup --cross-file "$cross" --buildtype release --strip \
        --prefix "$WORK/install.$arch" --bindir "x$arch" --libdir "x$arch" \
        "$WORK/build.$arch"
    ninja -C "$WORK/build.$arch" install
done

rm -rf "$OUT"
mkdir -p "$OUT/x64" "$OUT/x86"
cp "$WORK"/install.64/x64/*.dll "$OUT/x64/"
cp "$WORK"/install.86/x86/*.dll "$OUT/x86/"
echo "$STAMP" > "$OUT/stamp"
ls -l "$OUT"/x64 "$OUT"/x86
