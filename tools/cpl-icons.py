"""The Control Panel's icons and the devices in it, glossy as Aero is.

Put together from Arctic's own pictures (ICONS/), the Longhorn icon theme
(runtime/art/longhorn, as tools/compose-icons.py takes it) and a few pieces
drawn here (RCA plugs, a digital output, signal bars), with compose-icons.py's
helpers. Nothing is taken out of Windows.

They go to the modules: the Control Panel's items (Wine's applets through a
file of the same name in runtime/wine/modules, which takes the place of
Wine's own), imageres.dll by Windows' numbers for the folders whose
DefaultIcon names it, the Sound panel's and the volume mixer's devices, the
Network and Sharing Center's pictures.

usage: python tools/cpl-icons.py [--vendor LONGHORN_CLONE] [--preview OUT.png]
"""
import argparse
import importlib.util
import os
import shutil

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("compose_icons", os.path.join(ROOT, "tools", "compose-icons.py"))
ci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci)

mine = set()


def lh(ref):
    cat, name = ref.split("/", 1)
    mine.add((ci.CATS[cat], name))
    return ci.lh(ref)


# ---------------------------------------------------------------- drawn here

def rca(n):
    """a pair of RCA plugs, red and white, as a line input's"""
    c = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(c)
    for dx, top, bottom in ((0.12, (250, 110, 100), (170, 20, 20)), (0.5, (255, 255, 255), (175, 178, 185))):
        x = n * dx
        body = ci.vgrad(int(n * 0.3), int(n * 0.42), top + (255,), bottom + (255,))
        mask = Image.new("L", body.size, 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, body.size[0] - 1, body.size[1] - 1), radius=n * 0.06, fill=255)
        c.paste(body, (int(x), int(n * 0.38)), mask)
        d.rounded_rectangle((x, n * 0.38, x + n * 0.3, n * 0.8), radius=n * 0.06,
                            outline=tuple(v // 2 for v in bottom) + (255,), width=max(1, n // 48))
        # the metal sleeve and pin
        sleeve = ci.vgrad(int(n * 0.18), int(n * 0.16), (235, 235, 240, 255), (130, 135, 145, 255))
        c.paste(sleeve, (int(x + n * 0.06), int(n * 0.22)))
        d.rectangle((x + n * 0.13, n * 0.1, x + n * 0.17, n * 0.22), fill=(200, 170, 70, 255))
        # the cable
        d.rectangle((x + n * 0.1, n * 0.8, x + n * 0.2, n * 0.96), fill=(40, 40, 45, 255))
    ci.gloss_over(c, (0, 0, n, n))
    return c


def digital(n):
    """a digital output: a black box with a lit optical port"""
    c = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    box = (n * 0.06, n * 0.3, n * 0.94, n * 0.74)
    body = ci.vgrad(int(box[2] - box[0]), int(box[3] - box[1]), (95, 100, 110, 255), (20, 22, 26, 255))
    mask = Image.new("L", body.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, body.size[0] - 1, body.size[1] - 1), radius=n * 0.05, fill=255)
    c.paste(body, (int(box[0]), int(box[1])), mask)
    d = ImageDraw.Draw(c)
    d.rounded_rectangle(box, radius=n * 0.05, outline=(10, 10, 12, 255), width=max(1, n // 40))
    port = (n * 0.62, n * 0.44, n * 0.8, n * 0.6)
    d.rectangle(port, fill=(30, 30, 34, 255), outline=(150, 150, 160, 255), width=max(1, n // 64))
    d.ellipse((n * 0.67, n * 0.48, n * 0.75, n * 0.56), fill=(255, 60, 40, 255))
    d.rectangle((n * 0.16, n * 0.48, n * 0.5, n * 0.52), fill=(70, 75, 85, 255))
    ci.gloss_over(c, box)
    return c


def signal(level):
    """five bars, as many lit as the signal is strong"""
    def draw(n):
        c = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        d = ImageDraw.Draw(c)
        for i in range(5):
            x = n * (0.08 + i * 0.18)
            top = n * (0.72 - i * 0.15)
            lit = i < level
            col_top, col_bottom = ((140, 230, 120), (20, 130, 40)) if lit else ((235, 235, 238), (165, 168, 175))
            body = ci.vgrad(int(n * 0.13), int(n * 0.9 - top), col_top + (255,), col_bottom + (255,))
            c.paste(body, (int(x), int(top)))
            d.rectangle((x, top, x + n * 0.13, n * 0.9), outline=tuple(v * 2 // 3 for v in col_bottom) + (255,),
                        width=max(1, n // 48))
        return c
    return ci.drawn(draw)


# ---------------------------------------------------------------- what goes where

SOUND = lh("ap/cs-sound")
SPEAKERS = lh("d/audio-speakers")
HEADPHONES = lh("d/audio-headphones")
DISPLAY = lh("d/display")
NETWORK = lh("c/preferences-system-network")
WORKGROUP = lh("p/network-workgroup")
POWER = lh("ap/cs-power")
FOLDER_OPTIONS = ci.badge(lh("p/folder"), lh("c/preferences-system"), "br", 0.62)

M = "runtime/wine/modules/"
TARGETS = {
    # the Control Panel's items
    M + "dlls/powercpl/power.ico": POWER,
    M + "dlls/powercfg.cpl/power.ico": POWER,
    M + "dlls/powercpl/games.ico": lh("c/applications-games"),
    M + "dlls/usercpl/users.ico": lh("ap/cs-user-accounts"),
    M + "dlls/systemcpl/system.ico": lh("c/applications-system"),
    M + "dlls/notifications.cpl/notifications.ico": lh("ap/cs-notifications"),
    M + "dlls/joy.cpl/joy.ico": lh("d/input-gaming"),
    M + "dlls/appwiz.cpl/appwiz.ico": lh("ap/system-software-install"),
    M + "dlls/inetcpl.cpl/inetcpl.ico": lh("c/applications-internet"),
    "runtime/reactos/files/dll/cpl/desk/resources/applet.ico": lh("ap/cs-display"),
    # imageres.dll, by Windows' numbers
    M + "dlls/imageres/51.ico": lh("ap/cs-printer"),
    M + "dlls/imageres/77.ico": lh("ap/cs-fonts"),
    M + "dlls/imageres/80.ico": lh("ap/cs-panel"),
    M + "dlls/imageres/87.ico": lh("ap/system-software-install"),
    M + "dlls/imageres/109.ico": lh("ap/cs-display"),
    M + "dlls/imageres/114.ico": lh("c/preferences-desktop"),
    M + "dlls/imageres/149.ico": lh("c/applications-system"),
    M + "dlls/imageres/166.ico": FOLDER_OPTIONS,
    M + "dlls/imageres/9001.ico": NETWORK,
    # the Sound panel's devices
    M + "dlls/mmsys.cpl/mm3004.ico": SOUND,
    M + "dlls/mmsys.cpl/mm3010.ico": SPEAKERS,
    M + "dlls/mmsys.cpl/mm3011.ico": HEADPHONES,
    M + "dlls/mmsys.cpl/mm3012.ico": ci.drawn(rca),
    M + "dlls/mmsys.cpl/mm3013.ico": ci.drawn(digital),
    M + "dlls/mmsys.cpl/mm3014.ico": lh("d/audio-input-microphone"),
    M + "dlls/mmsys.cpl/mm3015.ico": lh("d/audio-headset"),
    M + "dlls/mmsys.cpl/mm3017.ico": DISPLAY,
    M + "dlls/mmsys.cpl/mm3018.ico": lh("d/audio-card"),
    M + "dlls/mmsys.cpl/mm3052.ico": lh("d/phone"),
    # the volume mixer's
    M + "programs/sndvol/sound.ico": SOUND,
    M + "programs/sndvol/speakers.ico": SPEAKERS,
    M + "programs/sndvol/headphones.ico": HEADPHONES,
    M + "programs/sndvol/digital.ico": ci.drawn(digital),
    M + "programs/sndvol/display.ico": DISPLAY,
    # the Network and Sharing Center's
    M + "dlls/netcenter/nc1.ico": WORKGROUP,
    M + "dlls/netcenter/nc6.ico": lh("d/computer"),
    M + "dlls/netcenter/nc7.ico": lh("c/gnome-globe"),
    M + "dlls/netcenter/nc8.ico": ci.PC2,
    M + "dlls/netcenter/nc14.ico": signal(0),
    M + "dlls/netcenter/nc15.ico": signal(1),
    M + "dlls/netcenter/nc16.ico": signal(2),
    M + "dlls/netcenter/nc17.ico": signal(3),
    M + "dlls/netcenter/nc18.ico": signal(4),
    M + "dlls/netcenter/nc19.ico": signal(5),
    M + "dlls/netcenter/nc22.ico": ci.badge(NETWORK, ci.PLUS, "br", 0.5),
    M + "dlls/netcenter/nc27.ico": ci.badge(WORKGROUP, ci.QUESTION, "br", 0.5),
    M + "dlls/netcenter/nc28.ico": lh("d/network-wired"),
}

SIZES = ci.BMP_SIZES + ci.PNG_SIZES + (256,)


def vendor(clone):
    for cat, name in sorted(mine):
        for s in ci.LH_SIZES:
            src = os.path.join(clone, "%dx%d" % (s, s), cat, name + ".png")
            if not os.path.exists(src) or not (src := ci.resolve(src)):
                continue
            dst = os.path.join(ci.LONGHORN, "%dx%d" % (s, s), cat, name + ".png")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
    print("vendored %d Longhorn pictures" % len(mine))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vendor")
    ap.add_argument("--preview")
    args = ap.parse_args()
    if args.vendor:
        vendor(args.vendor)
    sheet = []
    for rel, recipe in TARGETS.items():
        frames = [recipe(s) for s in SIZES]
        path = os.path.join(ROOT, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        ci.write_ico(path, frames)
        sheet.append((os.path.basename(path), frames[SIZES.index(48)], frames[SIZES.index(16)]))
    if args.preview:
        im = Image.new("RGBA", (len(sheet) * 60, 80), (255, 255, 255, 255))
        for i, (_, big, small) in enumerate(sheet):
            im.alpha_composite(big, (i * 60 + 6, 4))
            im.alpha_composite(small, (i * 60 + 22, 58))
        im.save(args.preview)
    print("%d icons" % len(sheet))


if __name__ == "__main__":
    main()
