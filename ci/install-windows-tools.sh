#!/bin/bash
# The tools every developer and agent reaches for, as their Windows builds
# (docs/linux-gate.md, stage 7): in Arctic nothing a person or a program runs
# is Linux, Linux itself is only behind wsl.exe.
#   PowerShell 7            C:\Program Files\PowerShell\7   (powershell.exe runs it)
#   Git for Windows         C:\Program Files\Git            (PortableGit: git, Git Bash)
#   GitHub CLI              C:\Program Files\GitHub CLI
# userinit puts their folders on the system PATH (programs/userinit), as their
# installers would. Each download is checked against its SHA-256.
#
# usage: install-windows-tools.sh PREFIX
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PFX=$1
PF="$PFX/drive_c/Program Files"
DL=${ARCTIC_DOWNLOADS:-/tmp/arctic-downloads}
mkdir -p "$DL"

PWSH_VERSION=7.6.6
PWSH_URL=https://github.com/PowerShell/PowerShell/releases/download/v$PWSH_VERSION/PowerShell-$PWSH_VERSION-win-x64.zip
PWSH_SHA256=02fe458be20493fbdf43f61ea20610b811ee6c738ab1676c61b9cfcd1a33c860
GIT_VERSION=2.56.0
GIT_URL=https://github.com/git-for-windows/git/releases/download/v$GIT_VERSION.windows.1/PortableGit-$GIT_VERSION-64-bit.7z.exe
GIT_SHA256=eceb5e061aa90df2f69ddd3e90f0030e1b8037a7829934bc40e4be1caa1accc1
GH_VERSION=2.102.0
GH_URL=https://github.com/cli/cli/releases/download/v$GH_VERSION/gh_${GH_VERSION}_windows_amd64.zip
GH_SHA256=ae64e556ecc240b200f7eba60d550e4bb60d78e860e69dd88c449405b86067f4

fetch() { # url sha256 -> path of the checked file
    local file="$DL/$(basename "$1")"
    if ! echo "$2  $file" | sha256sum -c --quiet - >/dev/null 2>&1; then
        curl -fsSL --retry 5 -o "$file" "$1"
        echo "$2  $file" | sha256sum -c --quiet - >&2
    fi
    echo "$file"
}

rm -rf "$PF/PowerShell/7" "$PF/Git" "$PF/GitHub CLI"
mkdir -p "$PF/PowerShell/7" "$PF/Git" "$PF/GitHub CLI"

7z x -y -o"$PF/PowerShell/7" "$(fetch "$PWSH_URL" "$PWSH_SHA256")" >/dev/null
7z x -y -o"$PF/Git" "$(fetch "$GIT_URL" "$GIT_SHA256")" >/dev/null
GH_TMP=$(mktemp -d)
7z x -y -o"$GH_TMP" "$(fetch "$GH_URL" "$GH_SHA256")" >/dev/null
cp -a "$GH_TMP/bin/gh.exe" "$PF/GitHub CLI/"
cp -a "$GH_TMP/LICENSE" "$PF/GitHub CLI/LICENSE.txt" 2>/dev/null || true
rm -rf "$GH_TMP"
ls -l "$PF/PowerShell/7/pwsh.exe" "$PF/Git/cmd/git.exe" "$PF/Git/bin/bash.exe" "$PF/GitHub CLI/gh.exe"

# Start, as their installers leave it
programs="$PFX/drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs"
mkdir -p "$programs/PowerShell" "$programs/Git"
python3 "$ROOT/ci/mklnk.py" "$programs/PowerShell/PowerShell 7 (x64).lnk" 'C:\Program Files\PowerShell\7\pwsh.exe'
python3 "$ROOT/ci/mklnk.py" "$programs/Git/Git Bash.lnk" 'C:\Program Files\Git\git-bash.exe' --cd-to-home
python3 "$ROOT/ci/mklnk.py" "$programs/Git/Git CMD.lnk" 'C:\Program Files\Git\git-cmd.exe' --cd-to-home
echo "PowerShell $PWSH_VERSION, Git for Windows $GIT_VERSION, GitHub CLI $GH_VERSION"
