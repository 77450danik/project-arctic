"""Draws the icons Arctic adds to ReactOS's shell32, as Windows 10 has them:
the blue star of Quick access and the Downloads folder (a folder with a blue
arrow). Writes 32-bit BMP icons at 16, 32 and 48 pixels.

usage: python tools/make-icons.py <ReactOS folder icon (shell32 res/icons/4.ico)> <output dir>
"""
import math
import os
import struct
import sys

from PIL import Image, ImageDraw

SIZES = (16, 32, 48)
BLUE = (0, 120, 215, 255)       # the Windows 10 accent
BLUE_DARK = (0, 84, 166, 255)


def star(size):
    scale = 8
    n = size * scale
    im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    cx, cy, r_out = n / 2, n / 2 + n * 0.03, n * 0.47
    r_in = r_out * 0.45
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        r = r_out if i % 2 == 0 else r_in
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    d = ImageDraw.Draw(im)
    d.polygon(pts, fill=BLUE, outline=BLUE_DARK, width=max(scale, n // 24))
    return im.resize((size, size), Image.LANCZOS)


def downloads(folder, size):
    im = folder.copy()
    scale = 8
    n = size * scale
    arrow = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(arrow)
    cx = n * 0.5
    top, shaft_w = n * 0.28, n * 0.16
    head_y, head_w, tip = n * 0.58, n * 0.44, n * 0.88
    pts = [(cx - shaft_w / 2, top), (cx + shaft_w / 2, top), (cx + shaft_w / 2, head_y),
           (cx + head_w / 2, head_y), (cx, tip), (cx - head_w / 2, head_y), (cx - shaft_w / 2, head_y)]
    d.polygon(pts, fill=BLUE, outline=(255, 255, 255, 255), width=max(scale, n // 20))
    arrow = arrow.resize((size, size), Image.LANCZOS)
    im.alpha_composite(arrow)
    return im


def write_ico(path, images):
    entries, blobs = [], []
    offset = 6 + 16 * len(images)
    for im in images:
        w, h = im.size
        bgra = im.convert("RGBA").tobytes("raw", "BGRA")
        rows = [bgra[y * w * 4:(y + 1) * w * 4] for y in range(h)][::-1]   # bottom-up
        mask_row = ((w + 31) // 32) * 4
        header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
        blob = header + b"".join(rows) + b"\0" * (mask_row * h)
        entries.append(struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(blob), offset))
        blobs.append(blob)
        offset += len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)) + b"".join(entries) + b"".join(blobs))


folder_ico, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
src = Image.open(folder_ico)
folders = {}
for s in SIZES:
    # the true-colour picture of each size, not a palette one
    best = max((e for e in src.ico.entry if e.width == s), key=lambda e: e.bpp)
    folders[s] = src.ico.getimage((s, s), best.bpp).convert("RGBA")
write_ico(os.path.join(out, "16800.ico"), [star(s) for s in SIZES])
write_ico(os.path.join(out, "16801.ico"), [downloads(folders[s], s) for s in SIZES])
preview = Image.new("RGBA", (140, 112), (255, 255, 255, 255))
x = 4
for s in SIZES:
    preview.alpha_composite(star(s), (x, 4))
    preview.alpha_composite(downloads(folders[s], s), (x, 58))
    x += s + 12
preview.save(os.path.join(out, "preview.png"))
print("icons written to", out)
