#!/bin/bash
# Puts the UEFI boot chain into a file tree (out/iso): Microsoft-signed shim as
# EFI\BOOT\BOOTX64.EFI, Limine signed with the Arctic key as grubx64.efi next
# to it, MokManager, and ARCTIC.cer, the certificate a PC enrolls once.
# Leaves out/efiboot.img, the same EFI\BOOT as a FAT image for the ISO's
# El Torito entry. Called by build-image.sh once limine.conf carries the
# hashes of the kernel and the initrd: the config's own hash is enrolled into
# Limine, so the config must not change after this.
#
# The key: ARCTIC_SB_KEY (the PEM itself, a CI secret) or ARCTIC_SB_KEY_FILE.
# Without either, a local build signs with a development key of its own, and
# a PC must enroll that key's ARCTIC.cer instead; CI refuses to.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${ARCTIC_OUT:-$ROOT/out}
TREE=${1:?usage: sign-efi.sh <tree with boot/limine/limine.conf>}
CERT="$ROOT/image/secureboot/arctic-sb.crt"
LIMINE_EFI=/usr/share/limine

# shim 16.1 from Debian 13: signed by the Microsoft UEFI CA 2011, which every
# Secure Boot PC trusts; MokManager is signed by Debian and shim trusts that.
# The pool keeps only current versions; snapshot.debian.org keeps every file
# by its SHA-1.
SHIM_DEBS=(
    "shim-signed/shim-signed_1.51~1+deb13u1+16.1-2~deb13u1_amd64.deb 3c802fa303c0e6bf126adee74028d1042e3120360d159c2c05181dc9b2f61005 622376cb844d11bbb2758011f48c6572564b6375"
    "shim-helpers-amd64-signed/shim-helpers-amd64-signed_1+16.1+2~deb13u1_amd64.deb 7e950d75d5a40ecc0a99f727452ce35780b47a04d9a2cc3b1ff7c2bb316c033e 5f224eaa14e6a4f3f6658e98f20349225abebabf"
)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
umask 077

# the key, and a check that it is the one ARCTIC.cer names
KEY="$WORK/arctic-sb.key"
if [ -n "${ARCTIC_SB_KEY:-}" ]; then
    printf '%s\n' "$ARCTIC_SB_KEY" > "$KEY"
elif [ -n "${ARCTIC_SB_KEY_FILE:-}" ]; then
    cp "$ARCTIC_SB_KEY_FILE" "$KEY"
elif [ -n "${GITHUB_ACTIONS:-}" ]; then
    echo "ARCTIC_SB_KEY is not set: the image would boot with Secure Boot on no PC"; exit 1
else
    DEV="$OUT/secureboot-dev"
    if [ ! -f "$DEV/arctic-sb.key" ]; then
        mkdir -p "$DEV"
        openssl req -new -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 -subj "/CN=Project Arctic development key" \
            -keyout "$DEV/arctic-sb.key" -out "$DEV/arctic-sb.crt" 2>/dev/null
    fi
    echo "WARNING: no ARCTIC_SB_KEY_FILE, signing with the development key in $DEV"
    echo "WARNING: with Secure Boot on, a PC must enroll this build's ARCTIC.cer, not the release one"
    cp "$DEV/arctic-sb.key" "$KEY"
    CERT="$DEV/arctic-sb.crt"
fi
[ "$(openssl x509 -in "$CERT" -noout -pubkey)" = "$(openssl pkey -in "$KEY" -pubout)" ] ||
    { echo "the signing key does not belong to $CERT"; exit 1; }
umask 022

# shim and MokManager, each package checked against its pinned hash
for entry in "${SHIM_DEBS[@]}"; do
    read -r path sha256 sha1 <<< "$entry"
    deb="$WORK/$(basename "$path")"
    curl -fsSL --retry 5 -o "$deb" "https://deb.debian.org/debian/pool/main/s/$path" ||
        curl -fsSL --retry 5 -o "$deb" "https://snapshot.debian.org/file/$sha1"
    echo "$sha256  $deb" | sha256sum -c --quiet
    bsdtar -xOf "$deb" 'data.tar.*' | bsdtar -xf - -C "$WORK"
done

# Limine with Arctic's patches (ci/build-limine.sh): Arch's stops at a
# prompt where the firmware refuses TPM measurements. Shim loads nothing
# without an .sbat section, and Limine's build has none. The section goes
# after the last one, where SizeOfImage ends.
bash "$ROOT/ci/build-limine.sh" "$WORK/limine-x64"
LIMINE_X64="$WORK/limine-x64/BOOTX64.EFI"
LIMINE_VER=$(limine --version | awk '{print $NF; exit}')
cat > "$WORK/sbat.csv" <<EOF
sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md
limine,1,Limine,limine,$LIMINE_VER,https://github.com/limine-bootloader/limine
limine.arctic,1,Project Arctic,limine,$LIMINE_VER-arctic.1,https://github.com/77450danik/project-arctic
EOF
pe_field() { objdump -p "$1" | awk -v f="$2" '$1 == f {print strtonum("0x" $2); exit}'; }
base=$(pe_field "$LIMINE_X64" ImageBase)
size=$(pe_field "$LIMINE_X64" SizeOfImage)
objcopy --add-section .sbat="$WORK/sbat.csv" --set-section-flags .sbat=contents,alloc,load,readonly,data \
    --change-section-vma .sbat=$((base + size)) "$LIMINE_X64" "$WORK/limine.efi"
objdump -h "$WORK/limine.efi" | grep -q ' \.sbat ' || { echo "no .sbat in Limine"; exit 1; }

# the config's hash goes into Limine; with Secure Boot on, Limine then
# refuses any config but this one and any file without a matching hash
limine enroll-config "$WORK/limine.efi" "$(b2sum "$TREE/boot/limine/limine.conf" | cut -d' ' -f1)"
sbsign --key "$KEY" --cert "$CERT" --output "$WORK/grubx64.efi" "$WORK/limine.efi"
sbverify --cert "$CERT" "$WORK/grubx64.efi"

# the tree: shim loads grubx64.efi and mmx64.efi from its own directory.
# BOOTIA32.EFI is Limine for 32-bit UEFI, which has no Secure Boot chain here.
ESP="$WORK/esp"
mkdir -p "$ESP/EFI/BOOT"
cp "$WORK/usr/lib/shim/shimx64.efi.signed" "$ESP/EFI/BOOT/BOOTX64.EFI"
cp "$WORK/usr/lib/shim/mmx64.efi.signed" "$ESP/EFI/BOOT/mmx64.efi"
cp "$WORK/grubx64.efi" "$ESP/EFI/BOOT/grubx64.efi"
cp "$LIMINE_EFI/BOOTIA32.EFI" "$ESP/EFI/BOOT/BOOTIA32.EFI"
openssl x509 -in "$CERT" -outform DER -out "$ESP/ARCTIC.cer"
mkdir -p "$TREE/EFI/BOOT"
cp -r "$ESP/." "$TREE/"

# the ISO's EFI system partition; MokManager finds ARCTIC.cer there, since
# firmware reads no ISO 9660
rm -f "$OUT/efiboot.img"
mkfs.fat -C -n ARCTICEFI "$OUT/efiboot.img" $(( $(du -sk --apparent-size "$ESP" | cut -f1) + 1024 )) >/dev/null
mcopy -s -Q -i "$OUT/efiboot.img" "$ESP"/* ::

shim_ver=${SHIM_DEBS[0]%%_amd64*}
echo "UEFI chain: shim-signed ${shim_ver##*_}, Limine $LIMINE_VER"
openssl x509 -in "$CERT" -noout -subject -fingerprint -sha256
