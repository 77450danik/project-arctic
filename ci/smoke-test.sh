#!/bin/bash
# Boots out/arctic.iso in QEMU (BIOS and UEFI), waits for the NT world to come
# up and takes a screenshot. Results go to out/test/.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ISO="$ROOT/out/arctic.iso"
RES="$ROOT/out/test"
MARKER="ARCTIC: NT ready"

sudo apt-get update -qq >/dev/null
sudo apt-get install -y -qq qemu-system-x86 ovmf >/dev/null
mkdir -p "$RES"

ACCEL=(-accel tcg)
TIMEOUT=1500
if [ -e /dev/kvm ]; then
    echo 'KERNEL=="kvm", GROUP="kvm", MODE="0666", OPTIONS+="static_node=kvm"' | sudo tee /etc/udev/rules.d/99-kvm.rules >/dev/null
    sudo udevadm control --reload-rules
    sudo udevadm trigger --name-match=kvm
    ACCEL=(-accel kvm -cpu host)
    TIMEOUT=600
fi
echo "accelerator: ${ACCEL[*]}"

boot() {
    local name=$1
    shift
    local log="$RES/$name-serial.log" qmp="$RES/$name.qmp"
    rm -f "$log" "$qmp"
    qemu-system-x86_64 "${ACCEL[@]}" -m 4096 -smp 2 -cdrom "$ISO" -vga std -display none \
        -serial "file:$log" -qmp "unix:$qmp,server=on,wait=off" "$@" &
    local pid=$! start=$SECONDS
    while ((SECONDS - start < TIMEOUT)); do
        grep -q "$MARKER" "$log" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 3
    done
    sleep 10
    python3 "$ROOT/ci/qmp-shot.py" "$qmp" "$RES/$name.ppm" "$RES/$name.png" || true
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    rm -f "$RES/$name.ppm"
    if grep -q "$MARKER" "$log"; then
        echo "== $name: OK after $((SECONDS - start)) s"
        grep "ARCTIC:" "$log"
        return 0
    fi
    echo "== $name: FAILED, last serial output:"
    tail -n 80 "$log"
    return 1
}

rc=0
boot bios || rc=1
boot uefi -bios /usr/share/ovmf/OVMF.fd || rc=1
exit $rc
