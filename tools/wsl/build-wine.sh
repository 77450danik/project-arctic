#!/bin/bash
# Incremental Wine build in the local WSL distro (same sources and flags as CI).
# The build tree lives inside the distro disk, not on /mnt/g, which is slow.
# Output: /root/arctic/out/wine
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export WINE_WORK=/root/arctic/wine WINE_OUT=/root/arctic/out/wine SKIP_DEPS=1
export CCACHE_DIR=/root/arctic/ccache
exec bash "$ROOT/ci/build-wine.sh"
