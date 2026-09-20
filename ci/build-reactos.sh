#!/bin/bash
# Builds the parts of ReactOS that Arctic takes: the shell (explorer.exe,
# shell32, browseui, shdocvw, uxtheme), the Mizu theme and the keyboard layouts. They are built with
# ReactOS's own toolchain (RosBE), the way ReactOS's CI builds them, and go
# to C: as native Windows binaries. Output: out/reactos, laid out as on C:.
#
# The commit is pinned in runtime/reactos/COMMIT; our changes to it are in
# runtime/reactos/patches. "build-reactos.sh toolchain" stops after RosBE.
set -euo pipefail

ROOT=${ARCTIC_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
WORK=${REACTOS_WORK:-$HOME/reactos}
ROSBE=${ROSBE_DIR:-$WORK/RosBE}
OUT=${REACTOS_OUT:-$ROOT/out/reactos}
COMMIT=$(cat "$ROOT/runtime/reactos/COMMIT")
ARCH=amd64
KEYBOARDS="kbdus kbdur kbdru"
TARGETS="explorer shell32 browseui shdocvw uxtheme mizu.msstyles $KEYBOARDS"

ROSBE_SCRIPT=https://gist.githubusercontent.com/zefklop/b2d6a0b470c70183e93d5285a03f5899/raw/build_rosbe_ci.sh
ROSBE_SCRIPT_SHA256=ea42b032fdf9b8b51e993ebe0d1aedc8714f7eff522f4a2fbac7b9aa32a9263c

mkdir -p "$WORK"
cd "$WORK"

# Toolchain, built once (the CI caches $ROSBE)
if [ ! -x "$ROSBE/RosBE.sh" ]; then
    wget -q -O build_rosbe_ci.sh "$ROSBE_SCRIPT"
    echo "$ROSBE_SCRIPT_SHA256  build_rosbe_ci.sh" | sha256sum -c -
    # the same fallback ReactOS's CI uses when the GNU mirror fails
    sed -i '/gcc-8.4.0\/gcc-8.4.0.tar.xz/c\wget https://ftpmirror.gnu.org/gcc/gcc-8.4.0/gcc-8.4.0.tar.xz || wget -L https://web.archive.org/web/2025if_/https://ftpmirror.gnu.org/gcc/gcc-8.4.0/gcc-8.4.0.tar.xz' build_rosbe_ci.sh
    bash build_rosbe_ci.sh "$ROSBE"
fi
[ "${1:-}" = toolchain ] && exit 0

# Source at the pinned commit, with our patches
if [ "$(cat src.commit 2>/dev/null)" != "$COMMIT" ]; then
    rm -rf src build
    git init -q src
    git -C src fetch -q --depth 1 https://github.com/reactos/reactos.git "$COMMIT"
    echo "$COMMIT" > src.commit
fi
git -C src checkout -q -f FETCH_HEAD 2>/dev/null || git -C src checkout -q -f "$COMMIT"
git -C src clean -qfdx
for patch in "$ROOT"/runtime/reactos/patches/*.patch; do
    [ -e "$patch" ] || continue
    echo "applying $(basename "$patch")"
    git -C src apply --whitespace=nowarn "$patch"
done

cat > build-steps.sh <<EOF
set -e
cmake -S "$WORK/src" -B "$WORK/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE:FILEPATH=toolchain-gcc.cmake \
    -DARCH:STRING=$ARCH -DCMAKE_BUILD_TYPE=Release -DDLL_EXPORT_VERSION=0x600
cmake --build "$WORK/build" --target $TARGETS
EOF
"$ROSBE/RosBE.sh" . 0 "$ARCH" < build-steps.sh

rm -rf "$OUT"
mkdir -p "$OUT/Windows/System32" "$OUT/Windows/Resources/Themes/Mizu"
cp build/base/shell/explorer/explorer.exe "$OUT/Windows/"
for dll in shell32 browseui shdocvw uxtheme; do
    cp "build/dll/win32/$dll/$dll.dll" "$OUT/Windows/System32/"
done
for kbd in $KEYBOARDS; do
    cp "build/dll/keyboard/$kbd/$kbd.dll" "$OUT/Windows/System32/"
done
cp build/media/themes/Mizu/mizu.msstyles/mizu.msstyles "$OUT/Windows/Resources/Themes/Mizu/"
echo "$COMMIT" > "$OUT/commit"
find "$OUT" -type f -exec ls -l {} +
