#!/bin/bash
# DXVK (Direct3D 8-11, DXGI) built in the local WSL distro, with the sources
# and flags of CI (ci/build-dxvk.sh). The work tree lives inside the distro
# disk and keeps its build directories, so a patch rebuilds what it touches.
# A private copy of the script runs, as for Wine.
# Output: /root/arctic/out/dxvk; build-rootfs.sh puts it in System32 and
# SysWOW64 of the image, make-update.sh --windows carries it to a C:.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export ARCTIC_ROOT=$ROOT ARCTIC_OUT=/root/arctic/out DXVK_WORK=/root/arctic/dxvk SKIP_DEPS=1
command -v meson >/dev/null && command -v ninja >/dev/null || pacman -S --noconfirm --needed meson ninja
mkdir -p /root/arctic
cp "$ROOT/ci/build-dxvk.sh" /root/arctic/build-dxvk.sh
exec bash /root/arctic/build-dxvk.sh
