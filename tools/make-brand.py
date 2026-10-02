"""Draws Arctic's branding from ARCTIC.png: the banner of the About dialog
(ShellAbout, winver) as ReactOS's rosbrand.dll carries it, 413 x 72 with a
5 high line under it, and the system's logo icon, IDI_WINLOGO of Wine's
user32, which ShellAbout shows when a program gives no icon of its own.

The snowflake is the logo's, white, its brightness taken as coverage. On the
banner it sits on the logo's black with "Arctic" beside it; the line under
it runs through the colours of the aurora on the default wallpaper. The icon
is the snowflake on a dark rounded tile, so that it shows on light dialogs
too.

usage: python tools/make-brand.py   (needs Pillow; runs in WSL)
  writes runtime/reactos/files/dll/branding/rosbrand/resources/bmp/*.bmp
  and runtime/wine/modules/dlls/user32/resources/oic_winlogo.ico
"""
import io
import os
import struct

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGO = os.path.join(ROOT, "ARCTIC.png")
BRAND = os.path.join(ROOT, "runtime", "reactos", "files", "dll", "branding", "rosbrand", "resources", "bmp")
USER32 = os.path.join(ROOT, "runtime", "wine", "modules", "dlls", "user32", "resources")
REGULAR = os.path.join(ROOT, "runtime", "fonts", "SourceSansPro-Regular.ttf")
SEMIBOLD = os.path.join(ROOT, "runtime", "fonts", "SourceSansPro-Semibold.ttf")
SS = 4  # supersampling

# the aurora of the wallpaper, left to right
AURORA = [(0x1f, 0xd1, 0x8f), (0x2a, 0x9d, 0xd8), (0x8a, 0x3f, 0xd1), (0xe0, 0x3c, 0xb4)]


def snowflake():
    """the logo's snowflake as an alpha mask, cropped to itself"""
    logo = Image.open(LOGO).convert("L")
    flake = logo.crop((0, 0, logo.width, int(logo.height * 0.74)))   # above "Project Arctic"
    flake = flake.point(lambda v: 0 if v < 24 else min(255, (v - 24) * 255 // 200))
    return flake.crop(flake.getbbox())


def flake_at(size):
    """the snowflake fitted in a square of that size, as white RGBA"""
    mask = snowflake()
    scale = size / max(mask.size)
    mask = mask.resize((max(1, round(mask.width * scale)), max(1, round(mask.height * scale))), Image.LANCZOS)
    out = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    white = Image.new("RGBA", mask.size, (255, 255, 255, 255))
    out.paste(white, ((size - mask.width) // 2, (size - mask.height) // 2), mask)
    return out


def write_bmp(path, im):
    """a 24-bit BMP, bottom-up, as rosbrand's are"""
    im = im.convert("RGB")
    w, h = im.size
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(h - 1, -1, -1):
        row = im.crop((0, y, w, y + 1)).tobytes("raw", "BGR")
        rows.append(row + b"\0" * (stride - len(row)))
    data = b"".join(rows)
    header = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 3780, 3780, 0, 0)
    with open(path, "wb") as f:
        f.write(header + info + data)


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


def banner():
    w, h = 413, 72
    big = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 255))
    flake = flake_at(56 * SS)
    big.alpha_composite(flake, (18 * SS, 8 * SS))
    draw = ImageDraw.Draw(big)
    draw.text((88 * SS, 34 * SS), "Arctic", font=ImageFont.truetype(REGULAR, 40 * SS), fill=(255, 255, 255, 255),
              anchor="lm")
    draw.text((214 * SS, 41 * SS), "pre-alpha", font=ImageFont.truetype(SEMIBOLD, 15 * SS),
              fill=(0x9f, 0xc6, 0xe8, 255), anchor="ls")
    return big.resize((w, h), Image.LANCZOS)


def bannerline():
    w, h = 413, 5
    line = Image.new("RGB", (w, h))
    for x in range(w):
        t = x / (w - 1) * (len(AURORA) - 1)
        i = min(int(t), len(AURORA) - 2)
        f = t - i
        c = tuple(int(AURORA[i][k] + (AURORA[i + 1][k] - AURORA[i][k]) * f) for k in range(3))
        for y in range(h):
            line.putpixel((x, y), c)
    return line


def logo_icon(size):
    big = Image.new("RGBA", (size * SS, size * SS), (0, 0, 0, 0))
    radius = size * SS * 0.18
    ImageDraw.Draw(big).rounded_rectangle((0, 0, size * SS - 1, size * SS - 1), radius, fill=(0x10, 0x16, 0x24, 255))
    inner = int(size * SS * 0.78)
    big.alpha_composite(flake_at(inner), ((size * SS - inner) // 2, (size * SS - inner) // 2))
    return big.resize((size, size), Image.LANCZOS)


os.makedirs(BRAND, exist_ok=True)
write_bmp(os.path.join(BRAND, "brand_banner.bmp"), banner())
write_bmp(os.path.join(BRAND, "brand_bannerline.bmp"), bannerline())
write_ico(os.path.join(USER32, "oic_winlogo.ico"), [logo_icon(s) for s in (16, 24, 32, 48, 256)])
print("written:", BRAND, os.path.join(USER32, "oic_winlogo.ico"))
