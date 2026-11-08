#!/bin/bash
# vkd3d-proton (Direct3D 12) built in the local WSL distro, with the sources
# and flags of CI (ci/build-vkd3d-proton.sh). The work tree lives inside the
# distro disk. A private copy of the script runs, as for Wine.
# Output: /root/arctic/out/vkd3d-proton; build-rootfs.sh puts it in System32
# and SysWOW64 of the image, make-update.sh --windows carries it to a C:.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export ARCTIC_ROOT=$ROOT ARCTIC_OUT=/root/arctic/out VKD3D_WORK=/root/arctic/vkd3d-proton \
    WINE_WORK=/root/arctic/wine SKIP_DEPS=1
command -v meson >/dev/null && command -v ninja >/dev/null || pacman -S --noconfirm --needed meson ninja
mkdir -p /root/arctic
cp "$ROOT/ci/build-vkd3d-proton.sh" /root/arctic/build-vkd3d-proton.sh
exec bash /root/arctic/build-vkd3d-proton.sh
