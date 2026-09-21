"""Boots an Arctic image with the serial console on a socket and runs shell
commands in the development shell (arctic#) that arctic-init keeps there.

usage: python tools/vm-shell.py [--iso FILE] [--append "kernel params"] [--after SECONDS]
                                [--keep] "command" ["command" ...]

Each command's output is printed. The VM is stopped at the end unless --keep.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.path.join(ROOT, "tools", "qemu", "qemu-system-x86_64.exe")
PORT = 4461

parser = argparse.ArgumentParser()
parser.add_argument("--iso", default=os.path.join(ROOT, "out", "arctic-local.iso"))
parser.add_argument("--append", default="")
parser.add_argument("--after", type=int, default=60, help="seconds to let it boot first")
parser.add_argument("--keep", action="store_true")
parser.add_argument("commands", nargs="*")
args = parser.parse_args()
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

cmd = [QEMU, "-accel", "whpx,kernel-irqchip=off", "-accel", "tcg", "-m", "4096", "-smp", "4",
       "-cdrom", args.iso, "-vga", "std", "-display", "none", "-device", "qemu-xhci",
       "-serial", "tcp:127.0.0.1:%d,server=on,wait=on" % PORT]
if args.append:
    kdir = os.path.join(ROOT, "out", "test-local", "kernel")
    os.makedirs(kdir, exist_ok=True)
    subprocess.run(["C:/Program Files/7-Zip/7z.exe", "e", "-y", "-bso0", "-o" + kdir, args.iso,
                    "arctic/vmlinuz", "arctic/initrd.img"], check=True)
    cmd += ["-kernel", os.path.join(kdir, "vmlinuz"), "-initrd", os.path.join(kdir, "initrd.img"),
            "-append", "console=ttyS0,115200 loglevel=6 arctic.dev=1 " + args.append]
vm = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(50):
    try:
        sock = socket.create_connection(("127.0.0.1", PORT))
        break
    except OSError:
        time.sleep(0.2)
sock.settimeout(0.5)
log = open(os.path.join(ROOT, "out", "test-local", "shell-serial.log"), "wb")


def read_for(seconds):
    data = b""
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
            log.write(chunk)
        except socket.timeout:
            pass
    return data.decode("utf-8", "replace")


read_for(args.after)
for c in args.commands:
    sock.sendall(c.encode() + b"\n")
    print("### " + c)
    print(read_for(6))
if not args.keep:
    vm.kill()
