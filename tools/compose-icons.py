"""The rest of shell32's icons, put together from the icons Arctic already has.

draw-icons.py places the icon set in ICONS/ (folders, documents, this PC,
drives, the network) and draws the few it lacks; everything else in
shell32's res/icons was still ReactOS's, Tango's look. Here each of those is
put together, one by one, from the same pieces: the set's own pictures, the
Longhorn icon theme (runtime/art/longhorn, Windows Longhorn's art: devices,
the recycle bin, printers, people, emblems) and a few small pieces drawn in
the same glossy manner (a plus, a gear, a moon, a bulb, a cable between two
computers). A network folder is the folder and the network; two computers
on a network are this PC twice with a cable between them.

Frames: 16, 20, 24, 30, 32, 40 and 48 pixels as BMP, 64 and 96 as PNG, and 256
for the icons the desktop shows. The icons of draw-icons.py and
make-icons.py are left alone.

usage: python tools/compose-icons.py [--vendor LONGHORN_CLONE] [--preview OUT.png] [ID ...]
  --vendor copies the Longhorn pictures the recipes use from a clone of
  github.com/B00merang-Artwork/Windows-Longhorn into runtime/art/longhorn
  (needs Pillow)
"""
import argparse
import io
import math
import os
import shutil
import struct

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SET = os.path.join(ROOT, "ICONS")
LONGHORN = os.path.join(ROOT, "runtime", "art", "longhorn")
OUT = os.path.join(ROOT, "runtime", "reactos", "files", "dll", "win32", "shell32", "res", "icons")
BOLD = os.path.join(ROOT, "runtime", "fonts", "SourceSansPro-Bold.ttf")
LH_SIZES = (16, 22, 32, 48, 128, 256)
BMP_SIZES = (16, 20, 24, 30, 32, 40, 48)
PNG_SIZES = (64, 96)
SS = 4  # supersampling of what is drawn here

# Longhorn's categories, as the recipes name them
CATS = {"d": "devices", "p": "places", "s": "status", "e": "emblems", "a": "actions",
        "m": "mimetypes", "ap": "apps", "c": "categories"}
used_longhorn = set()


# ---------------------------------------------------------------- sources

def scaled(im, size):
    if im.size == (size, size):
        return im.copy()
    return im.convert("RGBa").resize((size, size), Image.LANCZOS).convert("RGBA")


def st(name):
    """a picture of the set in ICONS/, at the size asked for"""
    def get(size):
        ico = Image.open(os.path.join(SET, name + ".ico"))
        entries = ico.ico.entry
        larger = [e for e in entries if e.width >= size]
        pick = (min(larger, key=lambda e: (e.width, -e.bpp)) if larger
                else max(entries, key=lambda e: (e.width, e.bpp)))
        im = ico.ico.getimage((pick.width, pick.height), pick.bpp).convert("RGBA")
        return scaled(im, size)
    return get


def lh(ref):
    """a picture of the Longhorn theme, "d/printer": the theme's own size if
    it has it, else the next larger one scaled down"""
    cat, name = ref.split("/", 1)
    used_longhorn.add((CATS[cat], name))

    def get(size):
        sizes = [s for s in LH_SIZES
                 if os.path.exists(os.path.join(LONGHORN, "%dx%d" % (s, s), CATS[cat], name + ".png"))]
        if not sizes:
            raise SystemExit("runtime/art/longhorn has no %s/%s (--vendor)" % (CATS[cat], name))
        pick = min([s for s in sizes if s >= size] or [max(sizes)])
        im = Image.open(os.path.join(LONGHORN, "%dx%d" % (pick, pick), CATS[cat], name + ".png")).convert("RGBA")
        return scaled(im, size)
    return get


def arrow(name, turn=0):
    """Arctic's own glossy arrows (runtime/art/arrows), turned"""
    def get(size):
        im = Image.open(os.path.join(ROOT, "runtime", "art", "arrows", name + ".png")).convert("RGBA")
        return scaled(im.rotate(turn), size)
    return get


def drawn(func):
    """a piece drawn here, supersampled"""
    def get(size):
        return scaled(func(size * SS), size)
    return get


# ---------------------------------------------------------------- putting together

def badge(base, piece, corner="br", part=0.55, minimum=9):
    """the piece over a corner of the base, as Windows puts a status on an icon"""
    def get(size):
        im = base(size)
        b = min(size, max(minimum, round(size * part)))
        p = piece(b)
        x = size - b if corner[1] == "r" else 0
        y = size - b if corner[0] == "b" else 0
        im.alpha_composite(p, (x, y))
        return im
    return get


def overlay(piece, part=0.5, minimum=8):
    """an overlay of the shell (shortcut, shared): the piece in the lower left
    corner of an empty icon, which the shell draws over another"""
    def get(size):
        im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        b = min(size, max(minimum, round(size * part)))
        im.alpha_composite(piece(b), (0, size - b))
        return im
    return get


def placed(piece, box):
    """the piece in a part of the icon: box (x, y, size) as fractions"""
    def get(size):
        im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        x, y, s = box
        b = max(1, round(size * s))
        im.alpha_composite(piece(b), (round(size * x), round(size * y)))
        return im
    return get


def layers(*parts):
    def get(size):
        im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        for part in parts:
            im.alpha_composite(part(size))
        return im
    return get


def folder_with(piece, part=0.6):
    """the set's folder with something in front of it, as Windows draws the
    folders of pictures or music"""
    return badge(st("Folder"), piece, "br", part, minimum=9)


def faded(base, alpha=0.55):
    def get(size):
        im = base(size)
        im.putalpha(im.getchannel("A").point(lambda v: int(v * alpha)))
        return im
    return get


# ---------------------------------------------------------------- drawn pieces

def vgrad(w, h, top, bottom):
    g = Image.new("RGBA", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4)))
    return g.resize((w, h))


def glossy_disc(n, top, bottom, edge):
    """a glossy disc, as the set's badges"""
    c = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    box = (n * 0.06, n * 0.06, n * 0.94, n * 0.94)
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).ellipse(box, fill=255)
    sh = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    sh.putalpha(mask.point(lambda v: v * 80 // 255))
    sh = sh.filter(ImageFilter.GaussianBlur(n * 0.03))
    c.alpha_composite(sh, (0, int(n * 0.03)))
    disc = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    disc.paste(vgrad(n, n, top, bottom), (0, 0), mask)
    c.alpha_composite(disc)
    ImageDraw.Draw(c).ellipse(box, outline=edge, width=max(1, n // 24))
    return c


def gloss_over(c, box):
    hl = Image.new("L", c.size, 0)
    x0, y0, x1, y1 = box
    ImageDraw.Draw(hl).ellipse((x0 + (x1 - x0) * 0.1, y0 + (y1 - y0) * 0.04,
                                x1 - (x1 - x0) * 0.1, y0 + (y1 - y0) * 0.5), fill=120)
    white = Image.new("RGBA", c.size, (255, 255, 255, 0))
    white.putalpha(hl)
    c.alpha_composite(white)


def plus_badge(n):
    c = glossy_disc(n, (130, 220, 95, 255), (35, 145, 40, 255), (20, 95, 25, 255))
    d = ImageDraw.Draw(c)
    w = n * 0.15
    d.rectangle((n * 0.5 - w / 2, n * 0.24, n * 0.5 + w / 2, n * 0.76), fill=(255, 255, 255, 255))
    d.rectangle((n * 0.24, n * 0.5 - w / 2, n * 0.76, n * 0.5 + w / 2), fill=(255, 255, 255, 255))
    gloss_over(c, (0, 0, n, n))
    return c


def pause_badge(n):
    c = glossy_disc(n, (255, 200, 80, 255), (220, 130, 10, 255), (150, 80, 0, 255))
    d = ImageDraw.Draw(c)
    for x in (0.34, 0.56):
        d.rectangle((n * x, n * 0.28, n * (x + 0.11), n * 0.72), fill=(255, 255, 255, 255))
    gloss_over(c, (0, 0, n, n))
    return c


def play_badge(n):
    c = glossy_disc(n, (130, 220, 95, 255), (35, 145, 40, 255), (20, 95, 25, 255))
    ImageDraw.Draw(c).polygon([(n * 0.38, n * 0.26), (n * 0.76, n * 0.5), (n * 0.38, n * 0.74)],
                              fill=(255, 255, 255, 255))
    gloss_over(c, (0, 0, n, n))
    return c


def letter_badge(text, top=(95, 170, 255, 255), bottom=(15, 80, 190, 255), edge=(10, 55, 140, 255)):
    def draw(n):
        c = glossy_disc(n, top, bottom, edge)
        font = ImageFont.truetype(BOLD, int(n * (0.62 if len(text) == 1 else 0.42)))
        ImageDraw.Draw(c).text((n * 0.5, n * 0.52), text, font=font, fill=(255, 255, 255, 255), anchor="mm")
        gloss_over(c, (0, 0, n, n))
        return c
    return draw


def gear(n):
    """a steel gear"""
    c = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    cx = cy = n / 2
    r_out, r_in, teeth = n * 0.47, n * 0.36, 8
    pts = []
    for i in range(teeth * 4):
        a = 2 * math.pi * i / (teeth * 4)
        r = r_out if i % 4 in (1, 2) else r_in
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    mask = Image.new("L", (n, n), 0)
    md = ImageDraw.Draw(mask)
    md.polygon(pts, fill=255)
    md.ellipse((cx - n * 0.14, cy - n * 0.14, cx + n * 0.14, cy + n * 0.14), fill=0)
    sh = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    sh.putalpha(mask.point(lambda v: v * 90 // 255))
    c.alpha_composite(sh.filter(ImageFilter.GaussianBlur(n * 0.025)), (0, int(n * 0.03)))
    body = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    body.paste(vgrad(n, n, (225, 232, 240, 255), (110, 125, 145, 255)), (0, 0), mask)
    c.alpha_composite(body)
    edge = mask.filter(ImageFilter.FIND_EDGES).point(lambda v: 255 if v > 40 else 0)
    line = Image.new("RGBA", (n, n), (60, 75, 95, 255))
    thick = (n // 48) | 1
    # Pillow's rank filters divide by zero at size 1
    line.putalpha(edge.filter(ImageFilter.MaxFilter(thick)) if thick >= 3 else edge)
    c.alpha_composite(line)
    gloss_over(c, (n * 0.05, n * 0.05, n * 0.95, n * 0.95))
    return c


def moon(n):
    c = glossy_disc(n, (70, 90, 160, 255), (15, 25, 70, 255), (10, 15, 45, 255))
    m = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(m)
    d.ellipse((n * 0.24, n * 0.2, n * 0.72, n * 0.68), fill=255)
    d.ellipse((n * 0.38, n * 0.12, n * 0.86, n * 0.6), fill=0)
    y = Image.new("RGBA", (n, n), (255, 220, 90, 255))
    y.putalpha(m)
    c.alpha_composite(y)
    gloss_over(c, (0, 0, n, n))
    return c


def bulb(n):
    c = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    mask = Image.new("L", (n, n), 0)
    d = ImageDraw.Draw(mask)
    d.ellipse((n * 0.18, n * 0.04, n * 0.82, n * 0.66), fill=255)
    d.polygon([(n * 0.32, n * 0.56), (n * 0.68, n * 0.56), (n * 0.62, n * 0.74), (n * 0.38, n * 0.74)], fill=255)
    glass = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    glass.paste(vgrad(n, n, (255, 250, 190, 255), (245, 185, 30, 255)), (0, 0), mask)
    c.alpha_composite(glass)
    dd = ImageDraw.Draw(c)
    dd.ellipse((n * 0.18, n * 0.04, n * 0.82, n * 0.66), outline=(170, 110, 0, 255), width=max(1, n // 40))
    for i in range(3):
        y = n * (0.76 + i * 0.065)
        dd.rounded_rectangle((n * 0.36, y, n * 0.64, y + n * 0.055), n * 0.02, fill=(150, 160, 175, 255),
                             outline=(80, 90, 105, 255), width=max(1, n // 64))
    gloss_over(c, (n * 0.18, n * 0.04, n * 0.82, n * 0.66))
    return c


def pie(n):
    c = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    box = (n * 0.06, n * 0.1, n * 0.94, n * 0.9)
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).ellipse(box, fill=255)
    sh = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    sh.putalpha(mask.point(lambda v: v * 80 // 255))
    c.alpha_composite(sh.filter(ImageFilter.GaussianBlur(n * 0.03)), (0, int(n * 0.03)))
    d = ImageDraw.Draw(c)
    d.pieslice(box, -90, 120, fill=(40, 120, 220, 255))
    d.pieslice(box, 120, 220, fill=(240, 170, 30, 255))
    d.pieslice(box, 220, 270, fill=(220, 60, 50, 255))
    d.ellipse(box, outline=(30, 50, 90, 255), width=max(1, n // 40))
    gloss_over(c, box)
    return c


def bars(n):
    c = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(c)
    cols = ((0.12, 0.5, (240, 170, 30)), (0.4, 0.22, (40, 120, 220)), (0.68, 0.38, (80, 190, 60)))
    for x, top, col in cols:
        box = (n * x, n * top, n * (x + 0.22), n * 0.9)
        body = vgrad(int(box[2] - box[0]) or 1, int(box[3] - box[1]) or 1,
                     tuple(min(255, v + 70) for v in col) + (255,), col + (255,))
        c.paste(body, (int(box[0]), int(box[1])))
        d.rectangle(box, outline=tuple(v // 2 for v in col) + (255,), width=max(1, n // 48))
    d.line((n * 0.06, n * 0.92, n * 0.94, n * 0.92), fill=(90, 100, 115, 255), width=max(1, n // 32))
    return c


def contrast(n):
    c = glossy_disc(n, (250, 250, 250, 255), (210, 210, 210, 255), (40, 40, 40, 255))
    m = Image.new("L", (n, n), 0)
    ImageDraw.Draw(m).pieslice((n * 0.06, n * 0.06, n * 0.94, n * 0.94), 90, 270, fill=255)
    k = Image.new("RGBA", (n, n), (20, 20, 20, 255))
    k.putalpha(m)
    c.alpha_composite(k)
    gloss_over(c, (0, 0, n, n))
    return c


def cable(n):
    """the network cable between two computers"""
    c = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(c)
    pts = [(n * 0.3, n * 0.62), (n * 0.3, n * 0.86), (n * 0.6, n * 0.86)]
    d.line(pts, fill=(40, 70, 110, 255), width=max(2, int(n * 0.07)), joint="curve")
    d.line(pts, fill=(110, 200, 255, 255), width=max(1, int(n * 0.035)), joint="curve")
    return c


def pages(base, count=3):
    """documents in a stack"""
    def get(size):
        im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        s = round(size * 0.8)
        step = (size - s) / max(1, count - 1)
        for i in range(count):
            im.alpha_composite(base(s), (round(i * step), round(i * step * 0.6)))
        return im
    return get


def text_on(base, text, color=(30, 90, 200, 255), box=(0.5, 0.6), part=0.5, font=BOLD):
    """a letter or two on a page, as Windows draws font files"""
    def get(size):
        im = base(size)
        big = scaled(im, size * SS)
        f = ImageFont.truetype(font, int(size * SS * part / max(1, len(text) * 0.6)))
        ImageDraw.Draw(big).text((size * SS * box[0], size * SS * box[1]), text, font=f, fill=color, anchor="mm")
        return scaled(big, size)
    return get


def lines_on(base):
    """text lines on the set's blank page"""
    def get(size):
        big = base(size * SS)
        d = ImageDraw.Draw(big)
        n = size * SS
        for i in range(6):
            y = n * (0.3 + i * 0.09)
            d.line((n * 0.3, y, n * (0.7 if i % 3 != 2 else 0.55), y), fill=(110, 130, 165, 255),
                   width=max(1, n // 40))
        return scaled(big, size)
    return get


def by_size(small, large, limit=32):
    """one picture up to a size, another above it: what reads at 16 pixels
    is not what shows at 48"""
    def get(size):
        return small(size) if size <= limit else large(size)
    return get


def two_computers():
    """two computers on a network: this PC twice and a cable between them;
    the set's own two monitors where that is too small to read"""
    pc = st("thisPC")
    return by_size(st("networkOKtray"),
                   layers(placed(pc, (0.0, 0.02, 0.62)), drawn(cable), placed(pc, (0.38, 0.4, 0.6))))


def power(n):
    """the power button, red as Windows draws Shut down"""
    c = glossy_disc(n, (255, 120, 95, 255), (190, 30, 20, 255), (120, 15, 10, 255))
    d = ImageDraw.Draw(c)
    w = max(2, int(n * 0.09))
    d.arc((n * 0.28, n * 0.3, n * 0.72, n * 0.74), 300, 240, fill=(255, 255, 255, 255), width=w)
    d.line((n * 0.5, n * 0.22, n * 0.5, n * 0.5), fill=(255, 255, 255, 255), width=w)
    gloss_over(c, (0, 0, n, n))
    return c


def sd_card(n):
    """a memory card: dark blue, the corner cut, gold contacts"""
    c = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    pts = [(n * 0.22, n * 0.06), (n * 0.66, n * 0.06), (n * 0.8, n * 0.2), (n * 0.8, n * 0.94), (n * 0.22, n * 0.94)]
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).polygon(pts, fill=255)
    sh = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    sh.putalpha(mask.point(lambda v: v * 80 // 255))
    c.alpha_composite(sh.filter(ImageFilter.GaussianBlur(n * 0.03)), (int(n * 0.02), int(n * 0.03)))
    body = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    body.paste(vgrad(n, n, (70, 110, 190, 255), (20, 40, 95, 255)), (0, 0), mask)
    c.alpha_composite(body)
    d = ImageDraw.Draw(c)
    d.polygon(pts, outline=(10, 20, 55, 255), width=max(1, n // 40))
    for i in range(5):
        x = n * (0.27 + i * 0.095)
        d.rectangle((x, n * 0.1, x + n * 0.06, n * 0.24), fill=(235, 190, 70, 255))
    d.rectangle((n * 0.28, n * 0.42, n * 0.74, n * 0.82), fill=(235, 240, 248, 255))
    d.rectangle((n * 0.28, n * 0.42, n * 0.74, n * 0.52), fill=(230, 90, 40, 255))
    gloss_over(c, (n * 0.22, n * 0.06, n * 0.8, n * 0.94))
    return c


def console(base):
    """the terminal with a prompt, as cmd's window shows it"""
    def get(size):
        big = base(size * SS)
        n = size * SS
        f = ImageFont.truetype(BOLD, int(n * 0.2))
        ImageDraw.Draw(big).text((n * 0.2, n * 0.42), "C:\\>_", font=f, fill=(225, 225, 225, 255), anchor="lm")
        return scaled(big, size)
    return get


# ---------------------------------------------------------------- the recipes

DOC = st("BlankFile")
FOLDER = st("Folder")
PC = st("thisPC")
HDD = st("harddisk")
NET = st("network")
SCREEN = st("desktop")
OK = st("yes_success_orapply")
NO = st("no_or_error")
WARN = st("warning")
RUN = st("RUN")
PICTURE = st("Picture")
MUSIC = st("music_2")
TEXT = lines_on(DOC)
PC2 = two_computers()

ERR = lh("s/dialog-error")
INFO = lh("s/dialog-information")
QUESTION = lh("s/dialog-question")
SEARCH = lh("a/system-search")
CLOCK = lh("a/appointment")
LOCK = lh("e/emblem-readonly")
USERS = lh("ap/system-users")
STAR = lh("s/starred")
SYNC = lh("e/emblem-synchronizing")
SHARED = lh("e/emblem-shared")
LINK = lh("e/emblem-symbolic-link")
MAIL = lh("e/emblem-mail")
EDIT = lh("ap/accessories-text-editor")
WINDOW = lh("m/exec")
TERMINAL = lh("ap/utilities-terminal")
BIN = lh("p/user-trash")
BIN_FULL = lh("s/user-trash-full")
PRINTER = lh("d/printer")
PRINTER_NET = lh("d/printer-network")
FAX = lh("d/modem")
DISC = lh("d/media-optical")
DISC_MUSIC = lh("d/media-optical-cd-audio")
DRIVE_OPTICAL = lh("d/drive-optical")
DRIVE_DVD = lh("d/media-optical-dvd")
FLOPPY = lh("d/media-floppy")
REMOVABLE = lh("d/drive-removable-media")
CARD = drawn(sd_card)
GO = lh("a/go-next")
ARROW_DOWN = arrow("up", 180)
GEAR = drawn(gear)
PLUS = drawn(plus_badge)
SHIELD = lh("s/dialog-password")
POWER = drawn(power)

DESKTOP_SIZE = (32, 33, 191, 192, 240, 15, 19, 17, 172, 20, 220, 269, 279, 12, 44)

RECIPES = {
    2: text_on(TEXT, "A", (200, 60, 40, 255), (0.62, 0.66), 0.42),        # RICH_TEXT
    6: FLOPPY,                                                          # 5_12_FLOPPY
    7: FLOPPY,                                                          # 3_14_FLOPPY
    10: badge(HDD, NET, "bl", 0.55),                                    # NETDRIVE
    11: badge(badge(HDD, NET, "bl", 0.55), ERR, "br", 0.5),             # NETDRIVE_OFF
    12: DRIVE_OPTICAL,                                                  # CDROM
    13: lh("d/drive-harddisk-solidstate"),                              # RAMDISK
    14: NET,                                                            # ENTIRE_NETWORK
    15: PC2,                                                            # NETWORK
    17: PRINTER,                                                        # PRINTER
    19: PC2,                                                            # COMPUTERS_NEAR_ME
    20: folder_with(WINDOW),                                            # PROGRAMS_FOLDER
    21: folder_with(CLOCK),                                             # RECENT_DOCUMENTS
    23: SEARCH,                                                         # SEARCH
    24: QUESTION,                                                       # HELP
    25: RUN,                                                            # RUN
    26: drawn(moon),                                                    # SLEEP
    27: badge(st("USB"), OK, "br", 0.6),                                # HARDWARE_REMOVE
    28: POWER,                                        # SHUTDOWN
    29: overlay(SHARED, 0.5),                                           # SHARE (overlay)
    30: overlay(LINK, 0.45),                                            # SHORTCUT (overlay)
    31: overlay(CLOCK, 0.5),                                            # FOLDER_WAIT (overlay)
    32: BIN,                                                            # EMPTY_RECYCLE_BIN
    33: BIN_FULL,                                                       # FULL_RECYCLE_BIN
    34: folder_with(NET),                                               # DIALUP_FOLDER
    37: folder_with(WINDOW),                                            # PROGRAMS_FOLDER2
    38: folder_with(PRINTER),                                           # PRINTERS_FOLDER
    39: folder_with(lh("m/font-x-generic")),                            # FONTS_FOLDER
    40: badge(SCREEN, WINDOW, "bl", 0.55),                              # TSKBAR_STARTMENU
    41: DISC_MUSIC,                                                     # CD_MUSIC
    42: pages(FOLDER),                                                  # TREE
    43: folder_with(PC),                                                # COMPUTER_FOLDER
    44: STAR,                                                           # FAVORITES
    45: lh("a/system-log-out"),                                         # LOGOFF
    46: folder_with(SEARCH),                                            # EXPLORER
    47: badge(NET, SYNC, "br", 0.55),                                   # UPDATE
    48: lh("a/system-lock-screen"),                                     # LOCKED
    49: badge(PC, ERR, "br", 0.5),                                      # DISCONN
    54: badge(HDD, ERR, "br", 0.5),                                     # NOT_CONNECTED_HDD
    133: pages(DOC),                                                    # MULTIPLE_FILES
    134: badge(DOC, WINDOW, "br", 0.6),                                 # OPEN_WITH
    135: badge(PC, SEARCH, "br", 0.6),                                  # FIND_COMPUTER
    138: PRINTER,                                                       # PRINTER2
    139: badge(PRINTER, PLUS, "br", 0.5),                               # PRINTER_ADD
    140: PRINTER_NET,                                                   # NET_PRINTER
    141: badge(PRINTER, DOC, "bl", 0.5),                                # FILE_PRINTER
    142: badge(BIN, DOC, "bl", 0.5),                                    # TRASH_FILE
    143: badge(BIN, FOLDER, "bl", 0.5),                                 # TRASH_FOLDER
    144: badge(badge(BIN, FOLDER, "bl", 0.5), DOC, "tl", 0.42),         # TRASH_FILE_FOLDER
    145: badge(pages(DOC, 2), ERR, "br", 0.5),                          # FILE_COPY_DELTE
    146: folder_with(GO, 0.55),                                         # FOLDER_MOVE1
    147: folder_with(EDIT, 0.55),                                       # FOLDER_RENAME
    148: folder_with(GEAR, 0.55),                                       # SETTINGS_FOLDER
    151: badge(TEXT, GEAR, "br", 0.5),                                  # INF_FILE
    152: TEXT,                                                          # TEXT_FILE
    153: badge(WINDOW, GEAR, "br", 0.55),                               # BAT_FILE
    154: badge(DOC, GEAR, "br", 0.55),                                  # SYSTEM_FILE
    155: lh("m/font-x-generic"),                                        # FONT_FILE
    156: lh("m/font_truetype"),                                         # TT_FONT_FILE
    157: lh("m/font_type1"),                                            # FONT_FILE2
    160: RUN,                                                           # RUN2
    161: BIN_FULL,                                                      # CONFIRM_DELETE
    165: badge(HDD, lh("a/document-save"), "br", 0.55),                 # TOOLS_BACKUP
    166: badge(HDD, SEARCH, "br", 0.55),                                # TOOLS_CHKDSK
    167: badge(HDD, drawn(bars), "br", 0.55),                           # TOOLS_DEFRAG
    168: badge(PRINTER, OK, "br", 0.5),                                 # PRINTER_OK
    169: badge(PRINTER_NET, OK, "br", 0.5),                             # NET_PRINTER_OK
    170: badge(badge(PRINTER, DOC, "bl", 0.5), OK, "br", 0.5),          # FILE_PRINTER_OK
    171: drawn(bars),                                                   # CHART
    172: folder_with(NET),                                              # NETWORK_FOLDER
    173: STAR,                                                          # FAVORITES2
    174: badge(DOC, lh("a/document-properties"), "br", 0.6),            # EXTENDED_PROPERTIES
    175: PC2,                                                           # NETWORK_CONNECTIONS
    176: badge(PC2, drawn(bars), "br", 0.5),                            # NETWORK_CHART
    177: badge(WINDOW, GEAR, "br", 0.55),                               # VIEW_SETTINGS
    178: badge(NET, drawn(bars), "br", 0.5),                            # INTERNET_CHART
    179: badge(PC, SYNC, "br", 0.55),                                   # COMPUTER_SYNC
    180: badge(SCREEN, WINDOW, "br", 0.6),                              # COMPUTER_WINDOW
    181: SCREEN,                                                        # COMPUTER_DESKTOP
    182: badge(WINDOW, lh("a/document-properties"), "bl", 0.5),         # SHOW_COMMON_TASKS
    183: WINDOW,                                                        # CLASSIC_FOLDERS
    184: WINDOW,                                                        # OPEN_IN_SOME_WINDOW
    185: pages(WINDOW, 2),                                              # OPEN_IN_NEW_WINDOW
    186: badge(DOC, lh("d/input-mouse"), "br", 0.6),                    # SINGLE_CLICK_TO_OPEN
    187: badge(DOC, lh("d/input-mouse"), "br", 0.6),                    # DOUBLE_CLICK_TO_OPEN
    191: BIN,                                                           # EMPTY_RECYCLE_BIN1
    192: BIN_FULL,                                                      # FULL_RECYCLE_BIN1
    193: folder_with(NET),                                              # WEB_FOLDERS
    194: SHIELD,                                                        # SECURITY
    196: FAX,                                                           # FAX
    197: badge(FAX, OK, "br", 0.5),                                     # FAX_OK
    198: badge(badge(FAX, NET, "bl", 0.5), OK, "br", 0.5),              # NET_FAX_OK
    199: badge(FAX, NET, "bl", 0.5),                                    # NET_FAX
    200: NO,                                                            # NO
    210: folder_with(GEAR, 0.55),                                       # FOLDER_OPTIONS
    220: USERS,                                                         # USERS2
    221: POWER,                                       # TURN_OFF
    222: DRIVE_DVD,                                                     # DVD_ROM
    223: pages(TEXT),                                                   # PAGES
    224: lh("m/video-x-generic"),                                       # MOVIE_FILE
    225: lh("m/audio-x-generic"),                                       # MUSIC_FILE
    226: lh("m/image-x-generic"),                                       # PICTURE_FILE
    227: badge(lh("m/video-x-generic"), MUSIC, "br", 0.5),              # MULTIMEDIA_FILE
    228: DISC_MUSIC,                                                    # CD_MUSIC2
    229: lh("d/input-mouse"),                                           # MOUSE
    231: ARROW_DOWN,                                                    # ARROWN_DOWN
    232: ARROW_DOWN,                                                    # ARROWN_DOWN1
    233: FLOPPY,                                                        # FDD
    234: faded(HDD),                                                    # NO_DRIVE
    240: BIN_FULL,                                                      # FULL_RECYCLE_BIN2
    241: badge(DOC, GO, "br", 0.5),                                     # FILE_MOVE
    242: badge(DOC, EDIT, "br", 0.6),                                   # RENAME
    243: pages(DOC),                                                    # FILES
    244: badge(NET, GO, "br", 0.5),                                     # MOVE_INTERNET
    245: PRINTER,                                                       # PRINTER3
    246: drawn(play_badge),                                             # ICON_246 (play)
    247: badge(NET, MUSIC, "br", 0.5),                                  # WEB_MUSIC
    248: lh("d/camera-photo"),                                          # CAMERA
    249: badge(PICTURE, drawn(play_badge), "br", 0.5),                  # SLIDE_SHOW
    250: SCREEN,                                                        # DISPLAY
    251: badge(NET, PICTURE, "br", 0.55),                               # WEB_PICS
    252: badge(PRINTER, PICTURE, "bl", 0.5),                            # PRINT_PICS
    253: badge(DOC, OK, "br", 0.55),                                    # FILE_CHECK
    254: badge(BIN, ERR, "br", 0.5),                                    # EMPTY_RECYCLE_BIN3
    255: lh("a/edit-undo"),                                             # ICON_255 (undo)
    256: folder_with(GO, 0.55),                                         # FOLDER_MOVE2
    257: PC2,                                                           # NETWORK_CONNECTIONS2
    258: badge(folder_with(NET), PLUS, "tr", 0.42),                     # NEW_NETWORK_FOLDER
    259: badge(PC2, lh("p/user-home"), "tr", 0.45),                     # NETWORK_HOME
    260: badge(DISC, EDIT, "br", 0.55),                                 # CD_EDIT
    261: badge(DISC, lh("a/edit-delete"), "br", 0.5),                   # CD_ERASE
    262: badge(DISC, ERR, "br", 0.5),                                   # CD_DELETE
    263: QUESTION,                                                      # HELP1
    264: folder_with(GO, 0.55),                                         # FOLDER_POINT
    265: lh("a/mail-message-new"),                                                          # SENDMAIL
    266: badge(DISC, GO, "br", 0.5),                                    # CD_POINT
    267: folder_with(SHARED, 0.55),                                     # SHARED_FOLDER
    268: lh("ap/access"),                                               # ACCESSIBILITY
    269: USERS,                                                         # USERS
    270: lh("ap/preferences-color"),                                    # SCREEN_COLORS
    271: lh("ap/system-software-install"),                              # ADD_REM_PROGRAMS
    272: badge(PRINTER, lh("d/input-mouse"), "br", 0.55),               # PRINTER_MOUSE
    273: badge(PC2, NET, "br", 0.5),                                    # INTERNET_STATUS
    274: GEAR,                                                          # SYSTEM_GEAR
    275: drawn(pie),                                                    # PIE_CHART
    276: badge(lh("ap/cs-date-time"), NET, "br", 0.45),                 # INTERNET_DATE
    277: lh("d/audio-speakers"),                                        # TUNES
    278: console(TERMINAL),                                                      # CMD
    279: USERS,                                                         # USER_ACCOUNTS
    280: badge(WINDOW, USERS, "br", 0.55),
    281: SEARCH,                                                        # WINDOWS_SEARCH
    282: badge(SCREEN, drawn(bulb), "br", 0.5),                         # COMPUTER_TEXT
    283: lh("d/input-keyboard"),                                        # OSK
    284: drawn(contrast),                                               # ICON_284 (high contrast)
    289: badge(TEXT, QUESTION, "br", 0.55),                             # HELP_FILE
    290: GO,                                                            # GO
    291: DRIVE_DVD,                                                     # DVD_DRIVE
    292: badge(DISC_MUSIC, PLUS, "br", 0.45),                           # CD_ADD_MUSIC
    293: DISC,                                                          # CD
    294: DISC,                                                          # CD_ROM
    295: DISC,                                                          # CDR
    296: badge(DISC, SYNC, "br", 0.42),                                 # CDRW
    297: DRIVE_DVD,                                                     # DVD_RAM
    298: DRIVE_DVD,                                                     # DVDR_ROM
    299: lh("d/multimedia-player"),                                     # MP3_PLAYER
    300: lh("p/network-server"),                                        # SERVER
    301: lh("p/network-server"),                                        # SERVER1
    302: DISC,                                                          # CD_ROM1
    303: CARD,                                                          # COMPACT_FLASH
    304: DRIVE_DVD,                                                     # DVD_ROM1
    305: FLOPPY,                                                        # FDD2
    307: CARD,                                                          # SD_MMC
    308: CARD,                                                          # SMART_MEDIA
    309: lh("d/camera-photo"),                                          # CAMERA1
    310: lh("d/phone"),                                                 # PHONE
    311: badge(PRINTER, NET, "bl", 0.5),                                # WEB_PRINTER
    312: REMOVABLE,                                                     # JAZ_DRIVE
    313: REMOVABLE,                                                     # ZIP_DRIVE
    314: lh("d/pda"),                                                   # PDA
    315: lh("d/scanner"),                                               # SCANNER
    316: badge(lh("d/scanner"), lh("d/camera-photo"), "tr", 0.5),       # IMAGING_DEVICES
    317: lh("d/camera-video"),                                          # CAMCORDER
    318: DRIVE_DVD,                                                     # DVDRW_ROM
    319: badge(FOLDER, STAR, "tr", 0.45),                               # NEW_FOLDER
    320: badge(DISC, GO, "br", 0.5),                                    # CD_POINT1
    322: STAR,                                                          # FAVOTITES
    323: SEARCH,                                                        # SEARCH1
    324: QUESTION,                                                      # HELP2
    325: lh("a/system-log-out"),                                        # LOGOFF1
    326: folder_with(WINDOW),                                           # PROGRAMS_FOLDER1
    327: folder_with(CLOCK),                                            # RECENT_DOCUMENTS1
    328: RUN,                                                           # RUN1
    329: POWER,                                       # SHUTDOWN1
    331: badge(NET, ERR, "br", 0.5),                                    # DISCONNECT
    337: folder_with(SEARCH),                                           # SEARCH_DIRECTORY
    338: badge(DOC, ERR, "br", 0.55),                                   # DELETE_PERMANENTLY
    512: NET,                                                           # WEB_BROWSER
    1001: drawn(bulb),                                                  # IDEA
    1002: badge(PRINTER, OK, "br", 0.5),                                # PRINTER_OK2
    1003: badge(lh("p/network-server"), OK, "br", 0.5),                 # SERVER_OK
    1004: badge(TEXT, QUESTION, "br", 0.55),                            # HELP_FILE1
    1005: badge(DOC, GO, "br", 0.5),                                    # FILE_MOVE1
    1006: badge(PRINTER, DOC, "bl", 0.5),                               # FILE_PRINT
    1007: badge(DOC, OK, "br", 0.55),                                   # FILE_OK
    1008: badge(PRINTER, drawn(pause_badge), "br", 0.5),                # PRINT_PAUSE
    1009: badge(PRINTER, drawn(play_badge), "br", 0.5),                 # PRINT_PLAY
    1010: badge(PRINTER, SHARED, "bl", 0.5),                            # PRINT_SHARE
    1011: FAX,                                                          # FAX2
    1700: folder_with(NET),
    8240: POWER,                                      # SHUTDOWN2
    16710: BIN,                                                         # DELETE1
    16715: BIN,                                                         # DELETE2
    16717: BIN,                                                         # DELETE3
    16718: BIN_FULL,                                                    # DELETE4
    16721: BIN_FULL,                                                    # DELETE5
}


# the other parts of ReactOS in the image, by their files under runtime/reactos/files
NET_CONNECTION = PC2
OTHERS = {
    # explorer.exe
    "base/shell/explorer/res/ico/100.ico": FOLDER,                      # IDI_MAIN
    "base/shell/explorer/res/ico/101.ico": FOLDER,                      # IDI_CABINET
    "base/shell/explorer/res/ico/102.ico": PRINTER,                     # IDI_PRINTER
    "base/shell/explorer/res/ico/103.ico": SCREEN,                      # IDI_DESKTOP
    "base/shell/explorer/res/ico/104.ico": badge(PRINTER, ERR, "br", 0.5),  # IDI_PRINTER_PROBLEM
    "base/shell/explorer/res/ico/107.ico": badge(SCREEN, WINDOW, "bl", 0.55),  # IDI_STARTMENU
    "base/shell/explorer/res/ico/108.ico": BIN,                         # IDI_RECYCLEBIN
    "base/shell/explorer/res/ico/109.ico": INFO,                        # IDI_SHOWINFO (balloons)
    "base/shell/explorer/res/ico/110.ico": WARN,                        # IDI_SHOWALERT
    "base/shell/explorer/res/ico/111.ico": NO,                          # IDI_SHOWERROR
    "base/shell/explorer/res/ico/205.ico": PC,                          # IDI_COMPUTER
    "base/shell/explorer/res/ico/250.ico": arrow("back"),               # IDI_ARROWLEFT
    "base/shell/explorer/res/ico/251.ico": arrow("forward"),            # IDI_ARROWRIGHT
    "base/shell/explorer/res/ico/252.ico": FOLDER,                      # IDI_FOLDER
    "base/shell/explorer/res/ico/253.ico": NET,                         # IDI_INTERNET
    "base/shell/explorer/res/ico/254.ico": lh("a/mail-message-new"),    # IDI_MAIL
    "base/shell/explorer/res/ico/256.ico": lh("a/mail-message-new"),    # IDI_MAILSMALL
    "base/shell/explorer/res/ico/257.ico": badge(SCREEN, WINDOW, "bl", 0.55),  # IDI_STARTMENU2
    # System Properties
    "dll/cpl/sysdm/resources/applet.ico": badge(PC, INFO, "br", 0.5),   # IDI_CPLSYSTEM
    "dll/cpl/sysdm/resources/devmgr.ico": USERS,                        # IDI_USERPROF
    "dll/cpl/sysdm/resources/hwprofile.ico": badge(PC, lh("ap/cs-drivers"), "br", 0.55),  # IDI_HARDPROF
    "dll/cpl/sysdm/resources/dev.ico": badge(PC, GEAR, "br", 0.55),     # IDI_DEVMGR
    "dll/cpl/sysdm/resources/license.ico": badge(TEXT, lh("s/security-high"), "br", 0.55),  # IDI_LICENSE
    "dll/cpl/sysdm/resources/up.ico": arrow("up"),                      # IDI_UP
    "dll/cpl/sysdm/resources/down.ico": ARROW_DOWN,                     # IDI_DOWN
    "dll/cpl/sysdm/resources/addhw.ico": badge(lh("ap/cs-cat-hardware"), PLUS, "br", 0.45),  # IDI_ADDHW
    # Display
    "dll/cpl/desk/resources/applet.ico": badge(SCREEN, lh("ap/preferences-color"), "br", 0.5),
    # Network Connections
    "dll/shellext/netshell/res/netshell.ico": folder_with(NET_CONNECTION),
    "dll/shellext/netshell/res/neterror.ico": badge(NET_CONNECTION, ERR, "br", 0.5),
    "dll/shellext/netshell/res/netidle.ico": NET_CONNECTION,
    "dll/shellext/netshell/res/netoff.ico": faded(NET_CONNECTION),
    "dll/shellext/netshell/res/netrec.ico": NET_CONNECTION,
    "dll/shellext/netshell/res/nettrans.ico": NET_CONNECTION,
    "dll/shellext/netshell/res/nettrrec.ico": NET_CONNECTION,
    "dll/shellext/netshell/res/netsetup.ico": badge(NET_CONNECTION, PLUS, "tr", 0.45),
    # the notification area's power, volume and Safely Remove Hardware
    "dll/shellext/stobject/resources/1.ico": lh("s/battery-full-charging"),
    "dll/shellext/stobject/resources/2.ico": badge(st("USB"), GO, "br", 0.5),
    "dll/shellext/stobject/resources/3.ico": lh("s/audio-volume-high"),
    "dll/shellext/stobject/resources/4.ico": lh("s/audio-volume-muted"),
    "dll/shellext/stobject/resources/battery/0.ico": lh("s/battery-empty"),
    "dll/shellext/stobject/resources/battery/1.ico": lh("s/battery-caution"),
    "dll/shellext/stobject/resources/battery/2.ico": lh("s/battery-low"),
    "dll/shellext/stobject/resources/battery/3.ico": lh("s/battery-good"),
    "dll/shellext/stobject/resources/battery/4.ico": lh("s/battery-good"),
    "dll/shellext/stobject/resources/battery/5.ico": lh("s/battery-full"),
    "dll/shellext/stobject/resources/battery/charging0.ico": lh("s/battery-empty-charging"),
    "dll/shellext/stobject/resources/battery/charging1.ico": lh("s/battery-caution-charging"),
    "dll/shellext/stobject/resources/battery/charging2.ico": lh("s/battery-low-charging"),
    "dll/shellext/stobject/resources/battery/charging3.ico": lh("s/battery-good-charging"),
    "dll/shellext/stobject/resources/battery/charging4.ico": lh("s/battery-full-charging"),
    "dll/shellext/stobject/resources/battery/error.ico": lh("s/battery-missing"),
    "dll/shellext/stobject/resources/battery/ac.ico": lh("d/ac-adapter"),
    "dll/shellext/stobject/resources/hotplug/0.ico": badge(st("USB"), WARN, "br", 0.6),
    # the picture viewer and Paint
    "dll/win32/shimgvw/res/main.ico": PICTURE,
    "dll/win32/shimgvw/res/bmp.ico": lh("m/image-x-generic"),
    "dll/win32/shimgvw/res/png.ico": lh("m/image-x-generic"),
    "base/applications/mspaint/img/paint.ico": lh("c/applications-graphics"),
}
FILES = os.path.join(ROOT, "runtime", "reactos", "files")


# ---------------------------------------------------------------- writing

def write_ico(path, frames):
    entries, blobs = [], []
    offset = 6 + 16 * len(frames)
    for im in frames:
        w, h = im.size
        if w > 48:
            buf = io.BytesIO()
            im.save(buf, "PNG", optimize=True)
            blob = buf.getvalue()
        else:
            bgra = im.convert("RGBA").tobytes("raw", "BGRA")
            rows = [bgra[y * w * 4:(y + 1) * w * 4] for y in range(h)][::-1]
            # the AND mask from alpha, for whoever draws without it
            alpha = im.getchannel("A")
            mask_rows = []
            stride = ((w + 31) // 32) * 4
            for y in range(h - 1, -1, -1):
                bits = bytearray(stride)
                for x in range(w):
                    if alpha.getpixel((x, y)) == 0:
                        bits[x // 8] |= 0x80 >> (x % 8)
                mask_rows.append(bytes(bits))
            blob = (struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0) +
                    b"".join(rows) + b"".join(mask_rows))
        entries.append(struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(blob), offset))
        blobs.append(blob)
        offset += len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(frames)) + b"".join(entries) + b"".join(blobs))


def resolve(path):
    """the theme links some pictures to others; a checkout without symbolic
    links has the link's target as the file's text"""
    for _ in range(8):
        with open(path, "rb") as f:
            head = f.read(512)
        if head.startswith(b"\x89PNG"):
            return path
        path = os.path.normpath(os.path.join(os.path.dirname(path), head.decode("utf-8").strip()))
        if not os.path.exists(path):
            return None
    return None


def vendor(clone):
    for cat, name in sorted(used_longhorn):
        for s in LH_SIZES:
            src = os.path.join(clone, "%dx%d" % (s, s), cat, name + ".png")
            if not os.path.exists(src) or not (src := resolve(src)):
                continue
            dst = os.path.join(LONGHORN, "%dx%d" % (s, s), cat, name + ".png")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
    print("vendored %d Longhorn pictures" % len(used_longhorn))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vendor")
    ap.add_argument("--preview")
    ap.add_argument("--others", action="store_true", help="only the other parts of ReactOS")
    ap.add_argument("ids", nargs="*", type=int)
    args = ap.parse_args()
    if args.vendor:
        vendor(args.vendor)
    jobs = []
    if not args.others:
        for icon_id in args.ids or sorted(RECIPES):
            sizes = BMP_SIZES + PNG_SIZES + ((256,) if icon_id in DESKTOP_SIZE else ())
            jobs.append((str(icon_id), RECIPES[icon_id], sizes, os.path.join(OUT, "%d.ico" % icon_id)))
    if args.others or not args.ids:
        for rel, recipe in OTHERS.items():
            name = os.path.splitext(os.path.basename(rel))[0]
            jobs.append((name, recipe, BMP_SIZES + PNG_SIZES, os.path.join(FILES, *rel.split("/"))))
    previews = []
    for name, recipe, sizes, path in jobs:
        frames = [recipe(s) for s in sizes]
        os.makedirs(os.path.dirname(path), exist_ok=True)
        write_ico(path, frames)
        previews.append((name, {s: f for s, f in zip(sizes, frames)}))
    ids = jobs
    if args.preview:
        cell = 120
        cols = 11
        rows = (len(previews) + cols - 1) // cols
        sheet = Image.new("RGBA", (cols * cell, rows * cell), (238, 238, 238, 255))
        d = ImageDraw.Draw(sheet)
        for i, (icon_id, frames) in enumerate(previews):
            x, y = (i % cols) * cell, (i // cols) * cell
            sheet.alpha_composite(frames[64], (x + 4, y + 4))
            sheet.alpha_composite(frames[32], (x + 72, y + 4))
            sheet.alpha_composite(frames[16], (x + 76, y + 42))
            sheet.alpha_composite(frames[24], (x + 72, y + 62))
            d.text((x + 4, y + 100), str(icon_id), fill=(0, 0, 0, 255))
        sheet.save(args.preview)
    print("composed %d icons" % len(ids))


if __name__ == "__main__":
    main()
