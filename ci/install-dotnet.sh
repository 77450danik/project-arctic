#!/bin/bash
# The .NET Framework 4.8, which Windows 11 has from the start (docs/dotnet.md).
# Microsoft's offline installer is downloaded and checked, as the other
# Windows programs of the image are, and its Windows Installer package is
# installed into C: by the image's own msiexec. The package is the one the
# installer itself runs on Windows 7 and 8.1; on Windows 10 and 11 the same
# files come with the system. Its mscoree.dll is the system's (Arctic's,
# Wine patch 0156), which hands .NET programs to the Framework's mscoreei.dll.
#
# usage: install-dotnet.sh ROOTFS     (the prefix is ROOTFS/var/tmp/prefix)
set -euo pipefail

RFS=$1
PFX="$RFS/var/tmp/prefix"
DL=${ARCTIC_DOWNLOADS:-/tmp/arctic-downloads}
mkdir -p "$DL"

NDP_URL=https://download.microsoft.com/download/f/3/a/f3a6af84-da23-40a5-8d1c-49cc10c8e76f/NDP48-x86-x64-AllOS-ENU.exe
NDP_SHA256=0a3a390c47e639d0f7fc65b21195fee6b7f65b066f80f70c60fab191d14b7e40

file="$DL/$(basename "$NDP_URL")"
if ! echo "$NDP_SHA256  $file" | sha256sum -c --quiet - >/dev/null 2>&1; then
    curl -fsSL --retry 5 -o "$file" "$NDP_URL"
    echo "$NDP_SHA256  $file" | sha256sum -c --quiet -
fi

# Z: is gone by now (build-rootfs.sh): the package goes through C:
TMP="$PFX/drive_c/windows/temp/ndp48"
rm -rf "$TMP"
mkdir -p "$TMP"
# the whole package but the system updates for Windows 8 and 10 (*.cab, 2 GB):
# the MSI takes some files (each language's eula.rtf) from beside itself
7z x -y -o"$TMP" "$file" -x'!*.cab' >/dev/null

cat > "$RFS/var/tmp/dotnet.sh" <<'EOF'
set -e
export NTPREFIX=/var/tmp/prefix HOME=/var/tmp/home USER=User LOGNAME=User LANG=uk_UA.UTF-8
# mscoree stays on: the package's own steps (ngen) are .NET programs
export NTDEBUG=-all NTDLLOVERRIDES="mshtml="
ntloader msiexec.exe /i 'C:\windows\temp\ndp48\netfx_Full_x64.msi' /qn /norestart
ntexec -w
EOF
timeout 1800 arch-chroot "$RFS" /usr/bin/busybox sh /var/tmp/dotnet.sh
rm -rf "$TMP" "$RFS/var/tmp/dotnet.sh"

for f in Framework64/v4.0.30319/clr.dll Framework/v4.0.30319/clr.dll Framework64/v4.0.30319/mscoreei.dll; do
    [ -f "$PFX/drive_c/windows/Microsoft.NET/$f" ] || { echo "install-dotnet: no C:\\Windows\\Microsoft.NET\\$f" >&2; exit 1; }
done
echo ".NET Framework 4.8"
