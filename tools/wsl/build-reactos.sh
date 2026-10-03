#!/bin/bash
# Builds Arctic's parts of ReactOS locally (ci/build-reactos.sh), so a ReactOS
# patch needs no CI run to be tried. It runs in the WSL distro "arctic-ros",
# Ubuntu 22.04 like the runner ReactOS's CI builds its toolchain on:
#   wsl -d arctic-ros -u root -- bash "/mnt/e/WORK_YT/project arctic/tools/wsl/build-reactos.sh"
# The first run installs the packages and builds RosBE (gcc 8.4, once, about
# an hour); after that the ReactOS tree in /root/reactos is built again by
# ninja, which compiles only what the patches touched.
# Output: out/reactos of this checkout, which tools/wsl/build-image.sh takes.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export ARCTIC_ROOT=$ROOT REACTOS_WORK=/root/reactos REACTOS_OUT=$ROOT/out/reactos

if [ ! -f /root/reactos-deps.done ]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q
    apt-get install -y -q build-essential git wget curl xz-utils bzip2 texinfo ninja-build \
        flex bison libgmp-dev libmpfr-dev libmpc-dev zlib1g-dev python3 cmake
    touch /root/reactos-deps.done
fi

# the script runs from a copy: switching branches meanwhile must not change it
cp "$ROOT/ci/build-reactos.sh" /root/build-reactos.sh
bash /root/build-reactos.sh "$@"
