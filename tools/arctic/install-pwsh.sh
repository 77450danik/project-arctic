#!/bin/bash
# PowerShell 7 (Microsoft's win-x64 zip, MIT) into C:\Program Files\PowerShell\7
# of an Arctic C:, where Claude Code's PowerShell tool looks first. It runs
# under Arctic's Wine (docs/updates.md, "PowerShell"); Wine's own
# powershell.exe is a stub that runs nothing.
# usage: install-pwsh.sh [C: as a path]   (default /mnt/c, Arctic's own C: in Arctic)
set -euo pipefail

VERSION=7.6.6
SHA256=02fe458be20493fbdf43f61ea20610b811ee6c738ab1676c61b9cfcd1a33c860
C=${1:-/mnt/c}
[ -d "$C/Windows/System32" ] || { echo "$C is not a C: drive"; exit 2; }

ZIP=$(mktemp --suffix=.zip)
trap 'rm -f "$ZIP"' EXIT
curl -fsSL -o "$ZIP" "https://github.com/PowerShell/PowerShell/releases/download/v$VERSION/PowerShell-$VERSION-win-x64.zip"
echo "$SHA256  $ZIP" | sha256sum -c --quiet
DEST="$C/Program Files/PowerShell/7"
rm -rf "$DEST.new"
mkdir -p "$DEST.new"
bsdtar -xf "$ZIP" -C "$DEST.new" 2>/dev/null || unzip -q "$ZIP" -d "$DEST.new"
rm -rf "$DEST"
mv "$DEST.new" "$DEST"
echo "PowerShell $VERSION in ${DEST#"$C"}"
