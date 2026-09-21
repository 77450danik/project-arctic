"""Boots an Arctic image with the serial console on a socket and runs shell
commands in the development shell (arctic#) that arctic-init keeps there.

usage: python tools/vm-shell.py [--iso FILE] [--append "kernel params"] [--after SECONDS]
                                [--put LOCAL:REMOTE] [--keep] "command" ["command" ...]

--put copies a local file into the VM (base64 over the console) before the commands.

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
parser.add_argument("--put", action="append", default=[], help="LOCAL:REMOTE")
parser.add_argument("--disk", action="append", default=[], help="a raw disk image attached as an internal disk")
parser.add_argument("commands", nargs="*")
args = parser.parse_args()
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

cmd = [QEMU, "-accel", "whpx,kernel-irqchip=off", "-accel", "tcg", "-m", "4096", "-smp", "4",
       "-drive", "if=none,id=cd,media=cdrom,format=raw,readonly=on,file=" + args.iso,
       "-device", "ide-cd,drive=cd,bootindex=1", "-vga", "std", "-display", "gtk", "-name", "Arctic",
       "-device", "qemu-xhci", "-device", "usb-tablet",
       "-serial", "tcp:127.0.0.1:%d,server=on,wait=on" % PORT]
for disk in args.disk:
    cmd += ["-drive", "if=virtio,format=raw,file=" + os.path.abspath(disk)]
if args.append:
    kdir = os.path.join(ROOT, "out", "test-local", "kernel")
    os.makedirs(kdir, exist_ok=True)
    subprocess.run(["C:/Program Files/7-Zip/7z.exe", "e", "-y", "-bso0", "-o" + kdir, args.iso,
                    "arctic/vmlinuz", "arctic/initrd.img"], check=True)
    cmd += ["-kernel", os.path.join(kdir, "vmlinuz"), "-initrd", os.path.join(kdir, "initrd.img"),
            "-append", "console=ttyS0,115200 loglevel=6 arctic.dev=1 " + args.append]
vm = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                      stderr=open(os.path.join(ROOT, "out", "test-local", "qemu-shell.log"), "w"))
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
for spec in args.put:
    local, remote = spec.rsplit(":", 1)
    import base64
    data = base64.encodebytes(open(local, "rb").read()).decode()
    sock.sendall(("busybox base64 -d > '%s' <<'__END__'\n" % remote).encode())
    for i in range(0, len(data), 4096):
        sock.sendall(data[i:i + 4096].encode())
        read_for(0.05)
    sock.sendall(b"__END__\n")
    read_for(3)
    print("### put %s -> %s" % (local, remote))
for c in args.commands:
    if c.startswith("#wait "):  # only read the console for that many seconds
        print(read_for(float(c.split()[1])))
        continue
    sock.sendall(c.encode() + b"\n")
    print("### " + c)
    print(read_for(6))
if not args.keep:
    vm.kill()
