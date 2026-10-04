"""Boots an Arctic ISO in the local QEMU (WHPX) in a visible window, waits for
the NT world, and saves the serial log and a screenshot.

usage: python tools/vm-test.py [image] [--wait SECONDS] [--append "kernel params"] [--script FILE]
                               [--usb] [--uefi] [--headless] [--keep] [--disk IMAGE] [--sound]

--usb boots out/arctic-usb.img as a USB stick and --uefi boots through OVMF, which
is how the image is tried the way real machines boot it. The stick is a qcow2
layer over the image (out/test-local/stick.qcow2): what one start writes, the
next one finds, until the image is rebuilt or --fresh is given.
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
parser.add_argument("--usb", action="store_true",
                    help="the image is a USB stick, not a CD; what it writes stays for the next start")
parser.add_argument("--stick-size", default="8G", help="how big the stick is (--usb)")
parser.add_argument("--boot-disk", help="boot from this raw disk image as an internal disk, no CD (an installed Arctic)")
parser.add_argument("--fresh", action="store_true", help="start from a freshly written stick (--usb)")
parser.add_argument("--stick-write", type=float, default=0,
                    help="MB/s the stick writes at, for a slow one (--usb); 20 writes a second at most")
parser.add_argument("--pull", action="store_true",
                    help="end by pulling the stick out (QEMU killed) instead of the power button (--usb)")
parser.add_argument("--no-reboot", action="store_true", help="QEMU exits when the guest restarts")
parser.add_argument("--initrd", help="with --append: this initrd instead of the image's (tools/wsl/make-dev-initrd.sh)")
parser.add_argument("--film", type=float, default=0,
                    help="seconds from power on to take a picture of every ~0.15 s (out/test-local/film)")
parser.add_argument("--uefi", action="store_true", help="boot through OVMF instead of the BIOS")
parser.add_argument("--monitors", type=int, default=1, help="how many monitors the card has (virtio-gpu above one)")
parser.add_argument("--resolution", help="WxH: the mode the monitor prefers (its EDID), e.g. 1920x1080")
parser.add_argument("--gpu", action="store_true",
                    help="3D on this PC's graphics card (virtio-gpu with virgl): dwm composes on a real GPU, "
                         "as on hardware, instead of llvmpipe on the CPU. Not working yet: with SDL on the "
                         "NVIDIA card the guest stops right after virtio-gpu starts")
parser.add_argument("--disk", action="append", default=[],
                    help="a raw disk image attached as an internal disk (repeatable)")
parser.add_argument("--sound", action="store_true",
                    help="two outputs that play through this PC's speakers: an HD Audio card and a USB one")
args = parser.parse_args()
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

os.makedirs(RES, exist_ok=True)
serial = os.path.join(RES, "serial.log")
shot = os.path.join(RES, "screen.png")
for path in (serial, shot):
    if os.path.exists(path):
        os.remove(path)

cmd = [QEMU, "-accel", "whpx,kernel-irqchip=off", "-accel", "tcg", "-m", "4096", "-smp", "4",
       "-display", "egl-headless" if args.headless and args.gpu else "none" if args.headless
       else "sdl,gl=on" if args.gpu else "gtk",
       "-name", "Arctic", "-device", "qemu-xhci,id=xhci", "-device", "usb-tablet", "-device", "usb-kbd", "-device", "usb-mouse",  # a USB keyboard, as real PCs have: the kernel repeats its keys
      
       "-serial", "file:" + serial, "-qmp", "tcp:127.0.0.1:4455,server=on,wait=off"]
if args.no_reboot:
    cmd += ["-no-reboot"]
if args.boot_disk:
    # an internal disk with Arctic installed (GPT, its own EFI partition)
    cmd += ["-drive", "if=none,id=bootdisk,format=raw,file=" + os.path.abspath(args.boot_disk),
            "-device", "virtio-blk-pci,drive=bootdisk,bootindex=0"]
elif args.usb:
    if args.iso.endswith(".iso") and os.path.exists(os.path.join(ROOT, "out", "arctic-usb.img")):
        args.iso = os.path.join(ROOT, "out", "arctic-usb.img")
    # The stick is a qcow2 layer over the image, as big as --stick-size: what
    # a start writes stays for the next one, the image stays as it was built.
    # A new image, or --fresh, starts from a freshly written stick.
    stick = os.path.join(RES, "stick.qcow2")
    built = "%s %d" % (os.path.abspath(args.iso), os.path.getmtime(args.iso))
    try:
        same = open(stick + ".src").read() == built
    except OSError:
        same = False
    if args.fresh or not same or not os.path.exists(stick):
        if os.path.exists(stick):
            os.remove(stick)
        subprocess.run([os.path.join(ROOT, "tools", "qemu", "qemu-img.exe"), "create", "-q", "-f", "qcow2",
                        "-b", os.path.abspath(args.iso), "-F", "raw", stick, args.stick_size], check=True)
        open(stick + ".src", "w").write(built)
        print("a freshly written %s stick" % args.stick_size)
    # --stick-write: a stick as slow to write as a cheap one (the ADATA that
    # froze: 2.3 MB/s, and few writes a second)
    slow = ""
    if args.stick_write:
        slow = ",throttling.bps-write=%d,throttling.iops-write=20" % (args.stick_write * 1000000)
    # --append boots the kernel directly, which takes boot index 0 itself
    cmd += ["-drive", "if=none,id=usbdisk,format=qcow2,file=" + stick + slow,
            "-device", "usb-storage,drive=usbdisk,bus=xhci.0" + ("" if args.append else ",bootindex=0")]
else:
    # the CD boots first even with other disks attached (--disk)
    cmd += ["-drive", "if=none,id=cd,media=cdrom,format=raw,readonly=on,file=" + args.iso,
            "-device", "ide-cd,drive=cd,bootindex=1"]
# One monitor is the standard VGA card, which is what a plain PC has; more
# than one needs virtio-gpu, whose heads QEMU shows as separate monitors.
if args.gpu:
    gpu = "virtio-vga-gl,id=gpu,max_outputs=%d" % args.monitors
    if args.resolution:
        xres, yres = args.resolution.lower().split("x")
        gpu += ",edid=on,xres=%s,yres=%s" % (xres, yres)
    cmd += ["-device", gpu, "-vga", "none"]
elif args.monitors > 1:
    cmd += ["-device", "virtio-gpu-pci,id=gpu,max_outputs=%d" % args.monitors, "-vga", "none"]
elif args.resolution:
    xres, yres = args.resolution.lower().split("x")
    cmd += ["-vga", "none", "-device", "VGA,edid=on,xres=%s,yres=%s" % (xres, yres)]
else:
    cmd += ["-vga", "std"]

for disk in args.disk:
    cmd += ["-drive", "if=virtio,format=raw,file=" + os.path.abspath(disk)]
if args.sound:
    cmd += ["-audiodev", "dsound,id=snd0", "-device", "intel-hda", "-device", "hda-duplex,audiodev=snd0",
            "-audiodev", "dsound,id=snd1", "-device", "usb-audio,audiodev=snd1,bus=xhci.0"]
if args.uefi:
    fw = os.path.join(ROOT, "tools", "qemu", "share")
    nvram = os.path.join(RES, "uefi-vars.fd")
    shutil.copyfile(os.path.join(fw, "edk2-i386-vars.fd"), nvram)
    cmd += ["-drive", "if=pflash,unit=0,format=raw,readonly=on,file=" + os.path.join(fw, "edk2-x86_64-code.fd"),
            "-drive", "if=pflash,unit=1,format=raw,file=" + nvram]
if args.append:
    kernel_dir = os.path.join(RES, "kernel")
    os.makedirs(kernel_dir, exist_ok=True)
    wsl = lambda p: "/mnt/" + p[0].lower() + p[2:].replace("\\", "/")
    root = ""
    if args.iso.endswith(".img"):
        # the stick: its EFI system partition, with mtools in the build distro;
        # C: is the partition its limine.conf names
        part = wsl(os.path.abspath(args.iso)) + "@@1M"
        for src, name in (("EFI/Arctic/vmlinuz", "vmlinuz"), ("EFI/Arctic/initrd.img", "initrd.img"),
                          ("boot/limine/limine.conf", "limine.conf")):
            # -n: no question before overwriting the copy from the last run
            subprocess.run(["wsl.exe", "-d", "arctic-build", "--", "mcopy", "-n", "-o", "-i", part, "::" + src,
                            wsl(os.path.join(kernel_dir, name))], check=True, stdin=subprocess.DEVNULL)
        found = re.search(r"arctic\.root=\S+", open(os.path.join(kernel_dir, "limine.conf")).read())
        root = found.group(0) + " " if found else ""
    elif not os.path.exists("C:/Program Files/7-Zip/7z.exe"):
        for name in ("vmlinuz", "initrd.img"):
            subprocess.run(["wsl.exe", "-d", "arctic-build", "--", "7z", "e", "-y", "-bso0", "-o" + wsl(kernel_dir),
                            wsl(os.path.abspath(args.iso)), "arctic/" + name], check=True)
    else:
        subprocess.run(["C:/Program Files/7-Zip/7z.exe", "e", "-y", "-bso0", "-o" + kernel_dir, args.iso,
                        "arctic/vmlinuz", "arctic/initrd.img"], check=True)
    cmd += ["-kernel", os.path.join(kernel_dir, "vmlinuz"),
            "-initrd", os.path.abspath(args.initrd) if args.initrd else os.path.join(kernel_dir, "initrd.img"),
            "-append", "console=ttyS0,115200 loglevel=6 arctic.dev=1 " + root + args.append]

start = time.time()
qemu_log = open(os.path.join(RES, "qemu.log"), "w")
vm = subprocess.Popen(cmd, stdout=qemu_log, stderr=subprocess.STDOUT)


def film(seconds):
    """The screen from power on, a picture every ~0.15 s, into out/test-local/film:
    to see the boot screen frame by frame (a logo that blinks, the spinner)."""
    import json
    import socket
    folder = os.path.join(RES, "film")
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    # Screendumps from the first moment slow QEMU down while OVMF looks for
    # the USB stick, and it then boots its shell instead: the film starts once
    # the firmware has picked what to boot (direct kernel boot has no such line)
    while not args.append and time.time() - start < 30 and vm.poll() is None:
        try:
            if "BdsDxe: starting" in open(serial, encoding="utf-8", errors="replace").read():
                break
        except OSError:
            pass
        time.sleep(0.1)
    for _ in range(100):
        try:
            sock = socket.create_connection(("127.0.0.1", 4455))
            break
        except OSError:
            time.sleep(0.1)
    else:
        return
    stream = sock.makefile("rw")
    stream.readline()

    def call(command, arguments=None):
        stream.write(json.dumps({"execute": command, **({"arguments": arguments} if arguments else {})}) + "\n")
        stream.flush()
        while "return" not in (reply := json.loads(stream.readline())) and "error" not in reply:
            pass

    call("qmp_capabilities")
    shots = []
    while time.time() - start < seconds and vm.poll() is None:
        ppm = os.path.join(folder, "%05.1f.ppm" % (time.time() - start))
        try:
            call("screendump", {"filename": ppm})
        except (OSError, ValueError):  # QEMU ended (--no-reboot)
            break
        shots.append(ppm)
        time.sleep(0.15)
    sock.close()
    time.sleep(0.5)
    for ppm in shots:
        if os.path.exists(ppm):
            subprocess.run([sys.executable, os.path.join(ROOT, "ci", "qmp-shot.py"), "--convert", ppm, ppm[:-4] + ".png"])
            os.remove(ppm)
    print("film: %d pictures in %s" % (len(shots), folder))


if args.film:
    import threading
    filming = threading.Thread(target=film, args=(args.film,))
    filming.start()
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
    if args.film:
        filming.join()  # QMP takes one client at a time
    time.sleep(args.wait)
    if args.script and "dwm ready" in seen:
        sys.path.insert(0, os.path.join(ROOT, "tools"))
        import vmscript
        size = tuple(int(n) for n in args.resolution.lower().split("x")) if args.resolution else None
        vmscript.run(vmscript.Qmp(heads=args.monitors, size=size), open(args.script, encoding="utf-8").read().splitlines())
    subprocess.run([sys.executable, os.path.join(ROOT, "ci", "qmp-shot.py"), "tcp:127.0.0.1:4455",
                    os.path.join(RES, "screen.ppm"), shot], check=False)
    # A stick is left the way a person leaves it: the power button, then
    # the machine shuts down by itself. Killing QEMU is pulling it out. A
    # press in the first seconds of the desktop gets lost (below csrss, it
    # never sees it), so it is pressed again, as a person would.
    if args.usb and not args.keep and not args.pull and vm.poll() is None and "dwm ready" in seen:
        import json
        import socket
        for press in range(1, 5):
            try:
                with socket.create_connection(("127.0.0.1", 4455), timeout=5) as qmp:
                    stream = qmp.makefile("rw")
                    stream.readline()
                    for command in ("qmp_capabilities", "system_powerdown"):
                        stream.write(json.dumps({"execute": command}) + "\n")
                        stream.flush()
                        stream.readline()
                vm.wait(timeout=15 if press < 4 else 60)
                print("shut down by the power button (press %d)" % press)
                break
            except (OSError, subprocess.TimeoutExpired) as e:
                if press == 4:
                    print("did not shut down by the power button:", type(e).__name__)
finally:
    if not args.keep and vm.poll() is None:
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
