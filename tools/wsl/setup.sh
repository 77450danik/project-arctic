#!/bin/bash
# One-time setup of the local build distro (WSL2, Arch Linux, "arctic-build").
# Run as root: wsl -d arctic-build -u root -- bash "/mnt/g/WORK_YT/project arctic/tools/wsl/setup.sh"
set -euo pipefail

# Builds must not pick up Windows tools, and the old E: entries in the
# Windows PATH only produce warnings
cat > /etc/wsl.conf <<'EOF'
[interop]
appendWindowsPath = false

[boot]
systemd = false
EOF

pacman-key --init
pacman-key --populate archlinux
sed -i 's/^#\?ParallelDownloads.*/ParallelDownloads = 8/' /etc/pacman.conf

# Everything ci/build-wine.sh and the image scripts need, plus ccache for
# incremental rebuilds
pacman -Syu --noconfirm --needed base-devel git ccache rsync mingw-w64-gcc perl \
    freetype2 gnutls alsa-lib vulkan-icd-loader vulkan-headers systemd-libs libusb \
    wayland libxkbcommon \
    arch-install-scripts squashfs-tools xorriso limine cpio zstd python-pillow noto-fonts ntfs-3g ntfsprogs

echo "cpus=$(nproc)"
free -m | awk '/Mem:/{print "ram=" $2 "MB"} /Swap:/{print "swap=" $2 "MB"}'
df -h / | tail -1
