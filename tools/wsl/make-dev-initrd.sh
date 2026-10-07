#!/bin/bash
# The initrd of the last image build with /init compiled again from this
# checkout: a change to the initrd tried in the VM in seconds, without
# building the image (vm-test.py --append ... --initrd <this file>).
# usage: make-dev-initrd.sh <out.img>   (as root in arctic-build)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:?usage: make-dev-initrd.sh <out.img>}
SRC=/root/arctic/out/initrd
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cp -a "$SRC/." "$WORK/"
gcc -static -Os -Wall -o "$WORK/init" "$ROOT/host/init/initrd-init.c" "$ROOT"/host/init/{bootanim,screen,stop,hiberfil}.c
(cd "$WORK" && find . | cpio -o -H newc --quiet) | zstd -19 -q -f -o "$OUT"
ls -l "$OUT"
