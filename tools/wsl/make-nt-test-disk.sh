#!/bin/bash
# The disk of Wine's conformance tests for NT's executive (docs/ntos.md
# "Перевірка", tests/vm/n1-ntos-tests.txt): the 64-bit *_test.exe of the
# Wine build tree, tests/vm/nt.cmd and the test programs of out/tests, on one
# FAT32 partition labelled TESTS (D: in the VM).
# Usage: make-nt-test-disk.sh OUTPUT.img
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${WINE_BUILD:-/root/arctic/wine/build}
img=${1:?output image}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

TESTS="ntdll kernel32 kernelbase advapi32 ws2_32 rpcrt4 ole32 shell32 msvcrt ucrtbase setupapi iphlpapi
       winhttp comctl32 dbghelp psapi secur32 win32u gdi32 user32"
mkdir "$TMP/files"
for t in $TESTS; do
    exe="$BUILD/dlls/$t/tests/x86_64-windows/${t}_test.exe"
    [ -f "$exe" ] || { echo "no $exe (make -C $BUILD dlls/$t/tests/x86_64-windows/${t}_test.exe)" >&2; exit 1; }
    cp "$exe" "$TMP/files/"
done
cp "$ROOT"/out/tests/ntbench.exe "$TMP/files/" 2>/dev/null || true
sed 's/\r*$/\r/' "$ROOT/tests/vm/nt.cmd" > "$TMP/files/nt.cmd"

mb=$(( $(du -cm "$TMP/files" | tail -1 | cut -f1) + 64 ))
rm -f "$img"
truncate -s $((mb + 2))M "$img"
sfdisk -q "$img" <<EOF
label: dos
start=1MiB, type=c
EOF
truncate -s ${mb}M "$TMP/fat"
mkfs.vfat -F 32 -n TESTS "$TMP/fat" >/dev/null
for f in "$TMP"/files/*; do
    mcopy -i "$TMP/fat" "$f" "::$(basename "$f")"
done
dd if="$TMP/fat" of="$img" bs=1M seek=1 conv=notrunc status=none
mdir -i "$TMP/fat" ::
