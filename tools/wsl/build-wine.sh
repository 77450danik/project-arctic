#!/bin/bash
# Incremental Wine build in the local WSL distro (same sources and flags as CI).
# The build tree lives inside the distro disk, not on /mnt/g, which is slow.
# Bash reads a script as it runs it, so a private copy is run: switching git
# branches on G: meanwhile must not change the script under a running build.
# Output: /root/arctic/out/wine
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export ARCTIC_ROOT=$ROOT WINE_WORK=/root/arctic/wine WINE_OUT=/root/arctic/out/wine SKIP_DEPS=1
export CCACHE_DIR=/root/arctic/ccache
mkdir -p /root/arctic
cp "$ROOT/ci/build-wine.sh" /root/arctic/build-wine.sh
exec bash /root/arctic/build-wine.sh
