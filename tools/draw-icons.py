"""Places the icon set in ICONS/ into ReactOS's shell32 and zipfldr and into
Wine's user32 (the message box icons), and draws, in the same glossy style,
the ones the set lacks: Control Panel, Desktop, Documents and Videos (the
set's folder with a page or a film strip), information and question.

usage: python tools/draw-icons.py
  writes runtime/reactos/files/... and runtime/wine/modules/dlls/user32/resources
"""
import io
import os
import shutil
import struct

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "ICONS")
FILES = os.path.join(ROOT, "runtime", "reactos", "files")
OUT = os.path.join(FILES, "dll", "win32", "shell32", "res", "icons")
USER32 = os.path.join(ROOT, "runtime", "wine", "modules", "dlls", "user32", "resources")
BOLD = os.path.join(ROOT, "runtime", "fonts", "SourceSansPro-Bold.ttf")
SIZES = (16, 24, 32, 48, 256)
SS = 4  # supersampling

# shell32 icon id <- file in ICONS/
PLACED = {
    1: "BlankFile.ico",       # IDI_SHELL_DOCUMENT
    3: "Application.ico",     # IDI_SHELL_EXE
    4: "Folder.ico",          # IDI_SHELL_FOLDER
    5: "Folder.ico",          # IDI_SHELL_FOLDER_OPEN
    8: "USB.ico",             # IDI_SHELL_REMOVEABLE
    9: "harddisk.ico",        # IDI_SHELL_DRIVE
    16: "thisPC.ico",         # IDI_SHELL_MY_COMPUTER
    18: "network.ico",        # IDI_SHELL_MY_NETWORK_PLACES
    236: "Picture.ico",       # IDI_SHELL_MY_PICTURES
    237: "music_2.ico",       # IDI_SHELL_MY_MUSIC
    16801: "Downloads.ico",   # IDI_SHELL_DOWNLOADS
}


def best_frame(path, size):
    """the set's picture at that size, the richest colour depth, scaled from the nearest"""
    ico = Image.open(path)
    entries = ico.ico.entry
    exact = [e for e in entries if e.width == size]
    pick = max(exact or entries, key=lambda e: (e.width >= size, -abs(e.width - size), e.bpp))
    im = ico.ico.getimage((pick.width, pick.height), pick.bpp).convert("RGBA")
    return im if im.size == (size, size) else im.resize((size, size), Image.LANCZOS)


def write_ico(path, images):
    entries, blobs = [], []
    offset = 6 + 16 * len(images)
    for im in images:
        w, h = im.size
        if w >= 256:
            buf = io.BytesIO()
            im.save(buf, "PNG")
            blob = buf.getvalue()
        else:
            bgra = im.convert("RGBA").tobytes("raw", "BGRA")
            rows = [bgra[y * w * 4:(y + 1) * w * 4] for y in range(h)][::-1]
            mask = b"\0" * (((w + 31) // 32) * 4 * h)
            blob = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0) + b"".join(rows) + mask
        entries.append(struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(blob), offset))
        blobs.append(blob)
        offset += len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)) + b"".join(entries) + b"".join(blobs))


def vgrad(size, top, bottom):
    w, h = size
    g = Image.new("RGBA", (1, h))
    for y in range(h):
        t = y / max(1, h - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4)))
    return g.resize((w, h))


def rounded(size, box, radius, fill_img):
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius, fill=255)
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    x0, y0, x1, y1 = [int(v) for v in box]
    layer.paste(fill_img.resize((x1 - x0, y1 - y0)), (x0, y0))
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    out.paste(layer, (0, 0), mask)
    return out, mask


def shadow(canvas, mask, offset, blur, alpha=90):
    sh = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    sh.putalpha(mask.point(lambda v: v * alpha // 255))
    sh = sh.filter(ImageFilter.GaussianBlur(blur))
    canvas.alpha_composite(sh, offset)


def gloss(canvas, box, radius):
    """the pale reflection over the upper half of a glossy shape"""
    x0, y0, x1, y1 = box
    hl, m = rounded(canvas.size, (x0 + 2, y0 + 2, x1 - 2, y0 + (y1 - y0) * 0.48), radius,
                    vgrad((10, 10), (255, 255, 255, 150), (255, 255, 255, 20)))
    canvas.alpha_composite(hl)


def control_panel(n):
    c = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    box = (n * 0.08, n * 0.12, n * 0.92, n * 0.88)
    panel, mask = rounded(c.size, box, n * 0.12, vgrad((10, 10), (110, 185, 255, 255), (20, 90, 200, 255)))
    shadow(c, mask, (int(n * 0.02), int(n * 0.03)), n * 0.03)
    c.alpha_composite(panel)
    d = ImageDraw.Draw(c)
    d.rounded_rectangle(box, n * 0.12, outline=(10, 60, 150, 255), width=max(1, n // 64))
    knobs = ((0.62, (255, 170, 30)), (0.36, (80, 200, 60)), (0.74, (235, 70, 60)))
    for i, (pos, col) in enumerate(knobs):
        y = n * (0.32 + i * 0.19)
        d.rounded_rectangle((n * 0.2, y - n * 0.025, n * 0.8, y + n * 0.025), n * 0.025, fill=(8, 40, 110, 200))
        kx = n * (0.2 + 0.6 * pos)
        r = n * 0.07
        d.ellipse((kx - r, y - r, kx + r, y + r), fill=col + (255,), outline=(255, 255, 255, 230),
                  width=max(1, n // 48))
    gloss(c, box, n * 0.12)
    return c


def desktop(n):
    c = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    box = (n * 0.06, n * 0.14, n * 0.94, n * 0.84)
    screen, mask = rounded(c.size, box, n * 0.06, vgrad((10, 10), (90, 170, 245, 255), (25, 85, 170, 255)))
    shadow(c, mask, (int(n * 0.02), int(n * 0.03)), n * 0.03)
    c.alpha_composite(screen)
    d = ImageDraw.Draw(c)
    d.rectangle((box[0] + n * 0.02, n * 0.72, box[2] - n * 0.02, n * 0.8), fill=(10, 30, 70, 220))
    d.ellipse((n * 0.11, n * 0.73, n * 0.17, n * 0.79), fill=(120, 200, 255, 255))
    for i in range(2):
        x = n * (0.16 + i * 0.16)
        d.rounded_rectangle((x, n * 0.24, x + n * 0.1, n * 0.34), n * 0.02, fill=(255, 255, 255, 220))
    d.rounded_rectangle(box, n * 0.06, outline=(10, 50, 120, 255), width=max(1, n // 64))
    gloss(c, box, n * 0.06)
    return c


def sign(n, glyph):
    """a glossy blue disc with a white glyph, as the set's error and warning signs"""
    c = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    box = (n * 0.08, n * 0.08, n * 0.92, n * 0.92)
    mask = Image.new("L", c.size, 0)
    ImageDraw.Draw(mask).ellipse(box, fill=255)
    shadow(c, mask, (0, int(n * 0.03)), n * 0.03)
    disc = Image.new("RGBA", c.size, (0, 0, 0, 0))
    disc.paste(vgrad(c.size, (95, 170, 255, 255), (15, 80, 190, 255)), (0, 0), mask)
    c.alpha_composite(disc)
    d = ImageDraw.Draw(c)
    d.ellipse(box, outline=(10, 55, 140, 255), width=max(1, n // 40))
    font = ImageFont.truetype(BOLD, int(n * 0.62))
    d.text((n * 0.5, n * 0.52), glyph, font=font, fill=(255, 255, 255, 255), anchor="mm")
    hl = Image.new("L", c.size, 0)
    ImageDraw.Draw(hl).ellipse((n * 0.16, n * 0.12, n * 0.84, n * 0.52), fill=110)
    white = Image.new("RGBA", c.size, (255, 255, 255, 0))
    white.putalpha(hl)
    c.alpha_composite(white)
    return c


def folder_with(n, overlay):
    c = best_frame(os.path.join(SRC, "Folder.ico"), n)
    c.alpha_composite(overlay(n))
    return c


def page(n):
    o = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(o)
    box = (n * 0.34, n * 0.3, n * 0.8, n * 0.86)
    d.rectangle(box, fill=(255, 255, 255, 255), outline=(120, 130, 145, 255), width=max(1, n // 48))
    for i in range(5):
        y = n * (0.4 + i * 0.085)
        d.line((n * 0.4, y, n * 0.74, y), fill=(110, 140, 190, 255), width=max(1, n // 40))
    return o


def film(n):
    o = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(o)
    box = (n * 0.3, n * 0.42, n * 0.86, n * 0.82)
    d.rectangle(box, fill=(35, 38, 45, 255))
    for i in range(6):
        x = n * (0.33 + i * 0.09)
        d.rectangle((x, n * 0.44, x + n * 0.05, n * 0.48), fill=(230, 230, 230, 255))
        d.rectangle((x, n * 0.76, x + n * 0.05, n * 0.8), fill=(230, 230, 230, 255))
    d.rectangle((n * 0.36, n * 0.52, n * 0.8, n * 0.72), fill=(90, 160, 230, 255))
    return o


def render(draw, size):
    return draw(size * SS).resize((size, size), Image.LANCZOS)


os.makedirs(OUT, exist_ok=True)
for icon_id, name in PLACED.items():
    shutil.copyfile(os.path.join(SRC, name), os.path.join(OUT, "%d.ico" % icon_id))
# the message box icons are user32's
os.makedirs(USER32, exist_ok=True)
shutil.copyfile(os.path.join(SRC, "no_or_error.ico"), os.path.join(USER32, "oic_hand.ico"))
shutil.copyfile(os.path.join(SRC, "warning.ico"), os.path.join(USER32, "oic_bang.ico"))
for name, glyph in (("oic_note.ico", "i"), ("oic_ques.ico", "?")):
    write_ico(os.path.join(USER32, name), [render(lambda n: sign(n, glyph), s) for s in SIZES])
# compressed folders are zipfldr's
os.makedirs(os.path.join(FILES, "dll", "shellext", "zipfldr", "res"), exist_ok=True)
shutil.copyfile(os.path.join(SRC, "ZIP.ico"), os.path.join(FILES, "dll", "shellext", "zipfldr", "res", "zipfldr.ico"))

drawn = {
    (22, 36, 137, 321, 330): lambda s: render(control_panel, s),        # IDI_SHELL_CONTROL_PANEL*
    (35,): lambda s: render(desktop, s),                                # IDI_SHELL_DESKTOP
    (235,): lambda s: folder_with(s, lambda n: render(page, n)),        # IDI_SHELL_MY_DOCUMENTS
    (238,): lambda s: folder_with(s, lambda n: render(film, n)),        # IDI_SHELL_MY_MOVIES
}
preview = Image.new("RGBA", (4 * 60 + 10, 60), (240, 240, 240, 255))
for i, (ids, draw) in enumerate(drawn.items()):
    frames = [draw(s) for s in SIZES]
    for icon_id in ids:
        write_ico(os.path.join(OUT, "%d.ico" % icon_id), frames)
    preview.alpha_composite(frames[3], (6 + i * 60, 6))
preview.save(os.path.join(ROOT, "out", "tmp", "drawn-icons.png"))
print("placed %d icons, drew %d" % (len(PLACED), sum(len(k) for k in drawn)))
