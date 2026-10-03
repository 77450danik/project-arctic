#!/bin/bash
# Builds the Arctic kernel locally in WSL, the same way as CI (ci/build-kernel.sh),
# with its own ccache, so that a change compiles only what it touches.
# The tree is unpacked anew each time at the same path; the sources are
# downloaded once per version. Output: /root/arctic/out/kernel, which
# tools/wsl/build-image.sh takes.
# The first run installs the packages the build needs (FIRST=1 does it again).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export KERNEL_OUT=/root/arctic/out/kernel KERNEL_WORK=/root/arctic/kbuild
export KERNEL_DOWNLOADS=/root/arctic/downloads CCACHE_DIR=/root/arctic/ccache-kernel
if [ -f /root/arctic/kernel-deps.done ] && [ -z "${FIRST:-}" ]; then
    export SKIP_DEPS=1
fi
mkdir -p /root/arctic
cp "$ROOT/ci/build-kernel.sh" /root/arctic/build-kernel.sh
# the copy runs from /root/arctic, but the checkout stays the root
sed -i "s|^ROOT=.*|ROOT=\"$ROOT\"|" /root/arctic/build-kernel.sh
bash /root/arctic/build-kernel.sh
touch /root/arctic/kernel-deps.done
