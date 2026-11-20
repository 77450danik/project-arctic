#!/bin/bash
# Builds the Arctic kernel locally in WSL, the same way as CI (ci/build-kernel.sh),
# with its own ccache, so that a change compiles only what it touches.
# The built tree is kept and only what changed is built, unless the patches,
# the config or the version changed: then it is unpacked anew at the same
# path (ccache serves what it can); the sources are
# downloaded once per version. Output: /root/arctic/out/kernel, which
# tools/wsl/build-image.sh takes.
# The first run installs the packages the build needs (FIRST=1 does it again).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export KERNEL_OUT=/root/arctic/out/kernel KERNEL_WORK=/root/arctic/kbuild
export KERNEL_DOWNLOADS=/root/arctic/downloads CCACHE_DIR=/root/arctic/ccache-kernel
# the built tree is kept while the patches and config are the same: a change
# to a driver compiles that driver (ci/build-kernel.sh)
export KERNEL_KEEP_TREE=1
if [ -f /root/arctic/kernel-deps.done ] && [ -z "${FIRST:-}" ]; then
    export SKIP_DEPS=1
fi
mkdir -p /root/arctic
cp "$ROOT/ci/build-kernel.sh" /root/arctic/build-kernel.sh
# the copy runs from /root/arctic, but the checkout stays the root
sed -i "s|^ROOT=.*|ROOT=\"$ROOT\"|" /root/arctic/build-kernel.sh
bash /root/arctic/build-kernel.sh
touch /root/arctic/kernel-deps.done
