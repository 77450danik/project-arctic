#!/bin/bash
# A disk with two tiny WSL distributions, for the VM (vm-test.py --disk):
# NTFS, labelled "WSL", with gate.vhdx and free.vhdx in its root. One stands
# for the gate distribution (arctic-build, behind UAC and the password) and
# the other for an ordinary one (arctic-ros, free), which is what the gate
# tests need: two different disks, so arctic-lxss cannot take them for the
# same distribution (host/init/arctic-lxss.c, is_gate_target).
#
# The rootfs inside is the smallest thing that answers "wsl -e id" and
# "wsl -e uname -sr": bash, a few tools of the build distribution and the
# libraries they need. Output: out/test-local/wsl-disk.img   (as root)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/out/test-local}
TOOLS="/bin/bash /usr/bin/id /usr/bin/uname /usr/bin/ls /usr/bin/cat /usr/bin/env /usr/bin/sleep"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$OUT"

# the rootfs: the programs, the libraries they are linked against, and the
# little the distribution needs to have a root user
mkdir -p "$W/rootfs"/{bin,usr/bin,usr/lib,lib64,etc,root,proc,sys,dev,mnt,tmp}
for tool in $TOOLS; do
    cp "$tool" "$W/rootfs/usr/bin/"
    ldd "$tool" | awk '{for (i = 1; i <= NF; i++) if ($i ~ /^\//) print $i}' >> "$W/libs"
done
sort -u "$W/libs" | while read -r lib; do
    case "$lib" in
        /lib64/*) cp -L "$lib" "$W/rootfs/lib64/" ;;
        *) cp -L "$lib" "$W/rootfs/usr/lib/" ;;
    esac
done
ln -sf usr/bin/bash "$W/rootfs/bin/bash"
ln -sf usr/bin/bash "$W/rootfs/bin/sh"
ln -sf usr/lib "$W/rootfs/lib"
cat > "$W/rootfs/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/bash
nobody:x:65534:65534:nobody:/:/bin/bash
EOF
cat > "$W/rootfs/etc/group" <<'EOF'
root:x:0:
nobody:x:65534:
EOF
echo "wsl-test" > "$W/rootfs/etc/hostname"

# the distribution's disk, as WSL keeps it: ext4 in a VHDX
truncate -s 192M "$W/ext4.img"
mkfs.ext4 -q -F -d "$W/rootfs" "$W/ext4.img"
qemu-img convert -f raw -O vhdx "$W/ext4.img" "$W/gate.vhdx"
cp "$W/gate.vhdx" "$W/free.vhdx"
ls -l "$W/gate.vhdx"

# the NTFS disk the two of them sit on, as a data disk of the machine
img="$OUT/wsl-disk.img"
rm -f "$img"
truncate -s 1G "$img"
sfdisk -q "$img" <<'EOF'
label: gpt
start=1MiB, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="WSL"
EOF
truncate -s 1022M "$W/part"
mkntfs -F -Q -q -L WSL "$W/part" 2>/dev/null
ntfscp -q "$W/part" "$W/gate.vhdx" gate.vhdx
ntfscp -q "$W/part" "$W/free.vhdx" free.vhdx
dd if="$W/part" of="$img" bs=1M seek=1 conv=notrunc status=none
ls -l "$img"
echo "the distributions are <letter>:\\gate.vhdx and <letter>:\\free.vhdx"
