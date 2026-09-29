#!/bin/bash
# Google Chrome in the image: the browser of the system (runtime/registry/
# chrome.reg makes it the default one), with shortcuts in Start and on the
# desktop. Google's installer needs its updater service, which does not run
# here, so the files are taken from the enterprise MSI the way that
# installer would lay them out: MSI -> setup -> updater.7z -> the Chrome
# installer -> chrome.7z -> Chrome-bin, which is Application\.
# Google Chrome's terms do not allow handing it on: an image with it is for
# the one who built it.
#
# usage: install-chrome.sh PREFIX
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PFX=$1
MSI_URL=https://dl.google.com/dl/chrome/install/googlechromestandaloneenterprise64.msi
APP="$PFX/drive_c/Program Files/Google/Chrome/Application"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

curl -fL --retry 5 -o "$WORK/chrome.msi" "$MSI_URL"
7z e -y -o"$WORK" "$WORK/chrome.msi" Binary.GoogleChromeInstaller >/dev/null
7z x -y -o"$WORK/setup" "$WORK/Binary.GoogleChromeInstaller" >/dev/null
7z x -y -o"$WORK/updater" "$WORK/setup/updater.7z" >/dev/null
installer=$(find "$WORK/updater" -name '*_chrome_installer.exe' | head -1)
7z x -y -o"$WORK/installer" "$installer" >/dev/null
7z x -y -o"$WORK/chrome" "$WORK/installer/chrome.7z" >/dev/null

mkdir -p "$APP"
cp -a "$WORK/chrome/Chrome-bin/." "$APP/"
echo "Chrome $(basename "$installer" _chrome_installer.exe)"

# no first-run pages and no question about the default browser: it is one
cat > "$APP/initial_preferences" <<'EOF'
{
  "distribution": {
    "skip_first_run_ui": true,
    "suppress_first_run_default_browser_prompt": true,
    "do_not_create_desktop_shortcut": true,
    "do_not_create_quick_launch_shortcut": true,
    "do_not_create_taskbar_shortcut": true,
    "make_chrome_default": false
  },
  "browser": { "check_default_browser": false }
}
EOF

# Start (all users) and the desktop (all users)
programs="$PFX/drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs"
desktop="$PFX/drive_c/users/Public/Desktop"
mkdir -p "$programs" "$desktop"
target='C:\Program Files\Google\Chrome\Application\chrome.exe'
python3 "$ROOT/ci/mklnk.py" "$programs/Google Chrome.lnk" "$target" --no-first-run
python3 "$ROOT/ci/mklnk.py" "$desktop/Google Chrome.lnk" "$target" --no-first-run
