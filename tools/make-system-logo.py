"""Draws the logo of the Control Panel's System page (systemcpl.dll), where
Windows 7 shows its edition's logo: the snowflake of ARCTIC.png in the
colours of the aurora on the default wallpaper, "Arctic" beside it in the
grey of Windows 7's logo. 32-bit BMPs with premultiplied alpha, for
AlphaBlend, at 100, 125, 150 and 200 %.

usage: python tools/make-system-logo.py   (needs Pillow)
  writes runtime/wine/modules/dlls/systemcpl/logo_*.bmp
"""
import os
import struct

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGO = os.path.join(ROOT, "ARCTIC.png")
OUT = os.path.join(ROOT, "runtime", "wine", "modules", "dlls", "systemcpl")
LIGHT = os.path.join(ROOT, "runtime", "fonts", "SourceSansPro-Regular.ttf")
SS = 4  # supersampling
HEIGHT = 64  # at 100 %

AURORA = [(0x1f, 0xd1, 0x8f), (0x2a, 0x9d, 0xd8), (0x8a, 0x3f, 0xd1)]
GREY = (0x5a, 0x5a, 0x5a)


def snowflake():
    logo = Image.open(LOGO).convert("L")
    flake = logo.crop((0, 0, logo.width, int(logo.height * 0.74)))
    flake = flake.point(lambda v: 0 if v < 24 else min(255, (v - 24) * 255 // 200))
    return flake.crop(flake.getbbox())


def aurora(w, h):
    """the aurora's colours from the top left to the bottom right"""
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            t = (x / max(w - 1, 1) + y / max(h - 1, 1)) / 2 * (len(AURORA) - 1)
            i = min(int(t), len(AURORA) - 2)
            f = t - i
            px[x, y] = tuple(int(AURORA[i][k] + (AURORA[i + 1][k] - AURORA[i][k]) * f) for k in range(3))
    return im


def logo(scale):
    h = HEIGHT * scale // 100 * SS
    mask = snowflake()
    mask = mask.resize((mask.width * h // mask.height, h), Image.LANCZOS)
    font = ImageFont.truetype(LIGHT, int(h * 0.62))
    text = "Arctic"
    box = ImageDraw.Draw(Image.new("L", (1, 1))).textbbox((0, 0), text, font=font)
    gap = h // 5
    w = mask.width + gap + (box[2] - box[0]) + SS * 2
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.paste(aurora(mask.width, h).convert("RGBA"), (0, 0), mask)
    draw = ImageDraw.Draw(out)
    draw.text((mask.width + gap - box[0], (h - (box[3] - box[1])) // 2 - box[1]), text, font=font, fill=GREY + (255,))
    return out.resize((w // SS, h // SS), Image.LANCZOS)


def write_bmp32(path, im):
    """BI_RGB 32 bit, bottom-up, premultiplied: AlphaBlend with AC_SRC_ALPHA"""
    w, h = im.size
    data = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = im.getpixel((x, y))
            data += bytes((b * a // 255, g * a // 255, r * a // 255, a))
    header = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, len(data), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(struct.pack("<2sIHHI", b"BM", 14 + len(header) + len(data), 0, 0, 14 + len(header)))
        f.write(header)
        f.write(data)


for scale in (100, 125, 150, 200):
    write_bmp32(os.path.join(OUT, f"logo_{scale}.bmp"), logo(scale))
print("written:", OUT)
