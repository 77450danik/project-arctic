#!/bin/bash
# Boots out/arctic.iso in QEMU and checks it. Results (serial logs and
# screenshots) go to out/test/.
#   bios, uefi   normal boot: the NT world must come up
#   uefi-sb, usb-sb  Secure Boot on, the Arctic key enrolled in MokList: the ISO,
#                and the USB image as a stick, must come up through shim
#   sb-denied    Secure Boot on, no Arctic key: shim must not start Limine
#   stop-initrd  stop screen raised on purpose in the initrd (no root filesystem yet)
#   stop-nt      stop screen raised on purpose by arctic-init once NT is up
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ISO="$ROOT/out/arctic.iso"
USB="$ROOT/out/arctic-usb.img"
KERNEL="$ROOT/out/iso/arctic/vmlinuz"
INITRD="$ROOT/out/iso/arctic/initrd.img"
RES="$ROOT/out/test"
READY="ARCTIC: NT ready"
STOP="ARCTIC: STOP"

sudo apt-get update -qq >/dev/null
sudo apt-get install -y -qq qemu-system-x86 ovmf >/dev/null
# virt-fw-vars writes the Secure Boot keys into the firmware variables
sudo apt-get install -y -qq python3-virt-firmware >/dev/null 2>&1 ||
    sudo pip3 install -q --break-system-packages virt-firmware
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

# boot <name> <marker that means success> [qemu args...]
boot() {
    local name=$1 want=$2
    shift 2
    local log="$RES/$name-serial.log" qmp="$RES/$name.qmp"
    rm -f "$log" "$qmp"
    qemu-system-x86_64 "${ACCEL[@]}" -m 4096 -smp 2 "${MEDIA[@]}" -vga std -display none \
        -serial "file:$log" -qmp "unix:$qmp,server=on,wait=off" "$@" &
    local pid=$! start=$SECONDS
    while ((SECONDS - start < TIMEOUT)); do
        grep -q "$want\|$STOP" "$log" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 3
    done
    sleep 12 # tasklist lines, or the stop screen reaching 100%
    python3 "$ROOT/ci/qmp-shot.py" "$qmp" "$RES/$name.ppm" "$RES/$name.png" || true
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    rm -f "$RES/$name.ppm" "$qmp"
    local ok=0
    case "$want" in
        "$STOP"*) grep -q "$want" "$log" && ok=1 ;;                          # the stop screen we asked for
        *) grep -q "$want" "$log" && ! grep -q "$STOP" "$log" && ok=1 ;;    # a normal boot, no stop screen
    esac
    if [ "$ok" = 1 ]; then
        echo "== $name: OK after $((SECONDS - start)) s"
        grep -a "ARCTIC:" "$log"
        return 0
    fi
    echo "== $name: FAILED, last serial output:"
    tail -n 80 "$log"
    return 1
}

MEDIA=(-cdrom "$ISO")
STICK=(-device qemu-xhci -drive if=none,id=stick,format=raw,snapshot=on,file="$USB"
    -device usb-storage,drive=stick,bootindex=0)

# Secure Boot firmware: OVMF with SMM, Microsoft's keys, and optionally the
# Arctic certificate in MokList, as MokManager leaves it on a real PC
secure() {
    local vars="$RES/$1-vars.fd" mok=()
    [ "${2:-}" = mok ] && mok=(--add-mok 605dab50-e046-4300-abb6-3dd810dd8b23 "$ROOT/image/secureboot/arctic-sb.crt")
    virt-fw-vars --input /usr/share/OVMF/OVMF_VARS_4M.ms.fd --output "$vars" --secure-boot "${mok[@]}" >/dev/null
    SECURE=(-machine q35,smm=on -global driver=cfi.pflash01,property=secure,value=on
        -drive if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.secboot.fd
        -drive if=pflash,format=raw,unit=1,file="$vars")
}

# denied <name> [qemu args...]: the kernel must not start within a minute
denied() {
    local name=$1 log="$RES/$1-serial.log"
    shift
    rm -f "$log"
    timeout 90 qemu-system-x86_64 "${ACCEL[@]}" -m 2048 "${MEDIA[@]}" -vga std -display none \
        -serial "file:$log" "$@"
    if grep -aq "Linux version\|ARCTIC:" "$log"; then
        echo "== $name: FAILED, the kernel started without an enrolled key"
        return 1
    fi
    echo "== $name: OK, nothing unsigned was started"
    grep -a "Verification failed\|Security Violation\|MokManager\|Failed to" "$log" | tr -d '\r' | head -n 5
    return 0
}

# Direct kernel boot, to pass extra kernel parameters
direct() {
    DIRECT=(-bios /usr/share/ovmf/OVMF.fd -kernel "$KERNEL" -initrd "$INITRD"
        -append "console=ttyS0,115200 loglevel=6 arctic.dev=1 $1")
}

rc=0
boot bios "$READY" || rc=1
boot uefi "$READY" -bios /usr/share/ovmf/OVMF.fd || rc=1
secure uefi-sb mok
boot uefi-sb "$READY" "${SECURE[@]}" || rc=1
MEDIA=("${STICK[@]}")
secure usb-sb mok
boot usb-sb "$READY" "${SECURE[@]}" || rc=1
secure sb-denied
denied sb-denied "${SECURE[@]}" || rc=1
MEDIA=(-cdrom "$ISO")
direct arctic.stoptest=initrd
boot stop-initrd "$STOP MANUALLY_INITIATED_CRASH" "${DIRECT[@]}" || rc=1
direct arctic.stoptest=nt
boot stop-nt "$STOP MANUALLY_INITIATED_CRASH" "${DIRECT[@]}" || rc=1
exit $rc
