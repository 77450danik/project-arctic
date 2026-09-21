"""Boots an Arctic ISO in the local QEMU (WHPX) in a visible window, waits for
the NT world, and saves the serial log and a screenshot.

usage: python tools/vm-test.py [image] [--wait SECONDS] [--append "kernel params"] [--script FILE]
                               [--usb] [--uefi] [--headless] [--keep] [--disk IMAGE]

--usb boots out/arctic-usb.img as a USB disk and --uefi boots through OVMF, which
is how the image is tried the way real machines boot it.
--append boots the kernel directly (taken from the ISO) with extra parameters,
e.g. --append arctic.stoptest=nt. --script runs input steps (tools/vmscript.py)
once the desktop is up. --disk adds an internal disk
(tools/wsl/make-test-disks.sh makes some). Results go to out/test-local/.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.path.join(ROOT, "tools", "qemu", "qemu-system-x86_64.exe")
RES = os.path.join(ROOT, "out", "test-local")
MARKER = re.compile(r"ARCTIC: (NT ready|dwm ready|STOP)")

parser = argparse.ArgumentParser()
parser.add_argument("iso", nargs="?", default=os.path.join(ROOT, "out", "arctic-local.iso"))
parser.add_argument("--wait", type=int, default=20, help="seconds after NT is up, before the screenshot")
parser.add_argument("--append", default="")
parser.add_argument("--timeout", type=int, default=240)
parser.add_argument("--headless", action="store_true", help="no window (the screenshot is still taken)")
parser.add_argument("--keep", action="store_true", help="leave the VM running after the test")
parser.add_argument("--script", help="input steps to run once the desktop is up (tools/vmscript.py)")
parser.add_argument("--usb", action="store_true", help="the image is a USB disk, not a CD")
parser.add_argument("--uefi", action="store_true", help="boot through OVMF instead of the BIOS")
parser.add_argument("--disk", action="append", default=[],
                    help="a raw disk image attached as an internal disk (repeatable)")
args = parser.parse_args()
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

os.makedirs(RES, exist_ok=True)
serial = os.path.join(RES, "serial.log")
shot = os.path.join(RES, "screen.png")
for path in (serial, shot):
    if os.path.exists(path):
        os.remove(path)

cmd = [QEMU, "-accel", "whpx,kernel-irqchip=off", "-accel", "tcg", "-m", "4096", "-smp", "4",
       "-vga", "std", "-display", "none" if args.headless else "gtk",
       "-name", "Arctic", "-device", "qemu-xhci,id=xhci", "-device", "usb-tablet",
       "-serial", "file:" + serial, "-qmp", "tcp:127.0.0.1:4455,server=on,wait=off"]
if args.usb:
    if args.iso.endswith(".iso") and os.path.exists(os.path.join(ROOT, "out", "arctic-usb.img")):
        args.iso = os.path.join(ROOT, "out", "arctic-usb.img")
    cmd += ["-drive", "if=none,id=usbdisk,format=raw,file=" + args.iso,
            "-device", "usb-storage,drive=usbdisk,bus=xhci.0,bootindex=0"]
else:
    # the CD boots first even with other disks attached (--disk)
    cmd += ["-drive", "if=none,id=cd,media=cdrom,format=raw,readonly=on,file=" + args.iso,
            "-device", "ide-cd,drive=cd,bootindex=1"]
for disk in args.disk:
    cmd += ["-drive", "if=virtio,format=raw,file=" + os.path.abspath(disk)]
if args.uefi:
    fw = os.path.join(ROOT, "tools", "qemu", "share")
    nvram = os.path.join(RES, "uefi-vars.fd")
    shutil.copyfile(os.path.join(fw, "edk2-i386-vars.fd"), nvram)
    cmd += ["-drive", "if=pflash,unit=0,format=raw,readonly=on,file=" + os.path.join(fw, "edk2-x86_64-code.fd"),
            "-drive", "if=pflash,unit=1,format=raw,file=" + nvram]
if args.append:
    kernel_dir = os.path.join(RES, "kernel")
    os.makedirs(kernel_dir, exist_ok=True)
    subprocess.run(["C:/Program Files/7-Zip/7z.exe", "e", "-y", "-bso0", "-o" + kernel_dir, args.iso,
                    "arctic/vmlinuz", "arctic/initrd.img"], check=True)
    cmd += ["-kernel", os.path.join(kernel_dir, "vmlinuz"), "-initrd", os.path.join(kernel_dir, "initrd.img"),
            "-append", "console=ttyS0,115200 loglevel=6 arctic.dev=1 " + args.append]

start = time.time()
qemu_log = open(os.path.join(RES, "qemu.log"), "w")
vm = subprocess.Popen(cmd, stdout=qemu_log, stderr=subprocess.STDOUT)
seen = set()
nt_up = 0
try:
    while time.time() - start < args.timeout and vm.poll() is None:
        time.sleep(1)
        if os.path.exists(serial):
            text = open(serial, encoding="utf-8", errors="replace").read()
            for m in MARKER.finditer(text):
                seen.add(m.group(1))
            if "STOP" in seen or "dwm ready" in seen:
                break
            if "NT ready" in seen:
                nt_up = nt_up or time.time()
                if time.time() - nt_up > 30:  # an image without dwm.exe
                    break
    print(f"markers after {time.time() - start:.0f} s: {sorted(seen) or 'none'}")
    time.sleep(args.wait)
    if args.script and "dwm ready" in seen:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import vmscript
        vmscript.run(vmscript.Qmp(), open(args.script, encoding="utf-8").read().splitlines())
    subprocess.run([sys.executable, os.path.join(ROOT, "ci", "qmp-shot.py"), "tcp:127.0.0.1:4455",
                    os.path.join(RES, "screen.ppm"), shot], check=False)
finally:
    if not args.keep:
        vm.kill()
    ppm = os.path.join(RES, "screen.ppm")
    if os.path.exists(ppm):
        os.remove(ppm)

for line in open(serial, encoding="utf-8", errors="replace"):
    if "ARCTIC:" in line or "arctic-init" in line or "dwm" in line.lower() or "csrss" in line:
        print(line.rstrip()[:200])
print("screenshot:", shot if os.path.exists(shot) else "none")
if vm.poll() is not None and vm.returncode:
    print("qemu exited with", vm.returncode, "-", open(os.path.join(RES, "qemu.log"), errors="replace").read()[-600:])
