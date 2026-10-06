#!/usr/bin/env python3
"""NVIDIA's user space from its .run package into the host root filesystem.

Arch keeps nvidia-utils only for the newest branch, but Pascal and older
(Maxwell, Volta) need the 580 branch, the last that drives them: its .run is
unpacked (sh NVIDIA-Linux-x86_64-<v>.run -x) and installed here the way
nvidia-installer would, by the package's own .manifest. Only the 64-bit parts
(WoW64 runs 32-bit programs on them), no X11, no systemd units, no manuals;
libglvnd itself comes from Arch (Mesa needs it too).

    install-nvidia-userspace.py <unpacked .run> <root>
"""

import os
import shutil
import sys

src, root = sys.argv[1], sys.argv[2]
LIB = os.path.join(root, 'usr/lib')

with open(os.path.join(src, '.manifest')) as f:
    lines = f.read().split('\n')
version = lines[1].strip()

# type: destination directory under the root
FILES = {
    'OPENGL_LIB': 'usr/lib', 'TLS_LIB': 'usr/lib', 'UTILITY_LIB': 'usr/lib', 'CUDA_LIB': 'usr/lib',
    'NVCUVID_LIB': 'usr/lib', 'ENCODEAPI_LIB': 'usr/lib', 'OPENCL_LIB': 'usr/lib',
    'VDPAU_LIB': 'usr/lib/vdpau',
    'GLVND_EGL_ICD_JSON': 'usr/share/glvnd/egl_vendor.d',
    'FIRMWARE': 'usr/lib/firmware/nvidia/' + version,
    'WINE_LIB': 'usr/lib/nvidia/wine',
    'APPLICATION_PROFILE': 'usr/share/nvidia',
    'OPENGL_DATA': 'usr/share/nvidia',
    'NVIDIA_MODPROBE': 'usr/bin',
}
SYMLINKS = {
    'OPENGL_SYMLINK': 'usr/lib', 'UTILITY_LIB_SYMLINK': 'usr/lib', 'CUDA_SYMLINK': 'usr/lib',
    'NVCUVID_LIB_SYMLINK': 'usr/lib', 'ENCODEAPI_LIB_SYMLINK': 'usr/lib', 'OPENCL_LIB_SYMLINK': 'usr/lib',
    'VDPAU_SYMLINK': 'usr/lib/vdpau', 'GBM_BACKEND_LIB_SYMLINK': 'usr/lib/gbm',
}
# the tools a running system uses; the rest serve X11, daemons and debugging
UTILITIES = {'nvidia-smi', 'nvidia-ngx-updater'}
# EGL platforms for Wayland and GBM, not X11
EGL_PLATFORMS = {'10_nvidia_wayland.json', '15_nvidia_gbm.json'}
# libraries of nvidia-settings (GTK), X11's EGL platforms, Vulkan SC,
# sandboxes, PKCS#11 and the CUDA debugger: nothing here uses them
SKIP = ('libnvidia-gtk', 'libnvidia-egl-xcb', 'libnvidia-egl-xlib', 'libnvidia-vksc-core',
        'libnvidia-sandboxutils', 'libnvidia-pkcs11', 'libcudadebugger', 'libnvidia-wayland-client')

count = 0
for line in lines[8:]:
    fields = line.split()
    if len(fields) < 3 or 'COMPAT32' in fields:
        continue
    name, perms, kind = fields[0], fields[1], fields[2]
    base = os.path.basename(name)
    if base.startswith(SKIP) or (len(fields) > 5 and os.path.basename(fields[-2]).startswith(SKIP)):
        continue
    dest = None
    if kind in FILES:
        dest = FILES[kind]
    elif kind == 'UTILITY_BINARY' and base in UTILITIES:
        dest = 'usr/bin'
    elif kind == 'VULKAN_ICD_JSON':
        # nvidia_icd.json to icd.d/, nvidia_layers.json to implicit_layer.d/
        sub = next((x for x in fields[3:] if x.endswith('/')), 'icd.d/')
        dest = 'usr/share/vulkan/' + sub.rstrip('/')
    elif kind == 'EGL_EXTERNAL_PLATFORM_JSON' and base in EGL_PLATFORMS:
        dest = 'usr/share/egl/egl_external_platform.d'
    elif kind in SYMLINKS:
        # name perms TYPE ARCH [path] target MODULE:x
        rest = [x for x in fields[4:] if not x.startswith('MODULE:') and x != '/']
        if not rest:
            continue
        target = rest[-1]
        d = os.path.join(root, SYMLINKS[kind])
        os.makedirs(d, exist_ok=True)
        if kind == 'GBM_BACKEND_LIB_SYMLINK':
            target = '../' + target
        link = os.path.join(d, base)
        if os.path.lexists(link):
            os.remove(link)
        os.symlink(target, link)
        count += 1
        continue
    if not dest:
        continue
    d = os.path.join(root, dest)
    os.makedirs(d, exist_ok=True)
    shutil.copy2(os.path.join(src, name), os.path.join(d, base))
    os.chmod(os.path.join(d, base), int(perms, 8) | 0o200)
    count += 1

# the sonames ldconfig would link (libnvidia-glcore.so.<v> needs none, the
# loaders look for the full name), plus the egl-wayland / egl-gbm platforms
for lib in os.listdir(LIB):
    for plat in ('libnvidia-egl-wayland.so.', 'libnvidia-egl-gbm.so.'):
        if lib.startswith(plat) and lib.count('.') == 4:
            so1 = plat + '1'
            if not os.path.lexists(os.path.join(LIB, so1)):
                os.symlink(lib, os.path.join(LIB, so1))

print(f'NVIDIA {version}: {count} files and links')
