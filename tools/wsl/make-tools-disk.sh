#!/bin/bash
# The test programs of out/tests on a disk of their own, for the VM
# (tools/vm-test.py --disk out/test-local/tools.img): one FAT32 partition
# labelled TESTS, which Arctic mounts as the next drive letter (D:), so a
# script can start them from Run.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/out/test-local"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

img="$OUT/tools.img"
rm -f "$img"
truncate -s 42M "$img"
sfdisk -q "$img" <<EOF
label: dos
start=1MiB, type=c
EOF
truncate -s 40M "$TMP/fat"
mkfs.vfat -F 32 -n TESTS "$TMP/fat" >/dev/null
for f in "$ROOT"/out/tests/*.exe; do
    mcopy -i "$TMP/fat" "$f" "::$(basename "$f")"
done
dd if="$TMP/fat" of="$img" bs=1M seek=1 conv=notrunc status=none
mdir -i "$TMP/fat" ::
