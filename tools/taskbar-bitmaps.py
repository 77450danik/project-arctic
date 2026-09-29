#!/usr/bin/env python3
"""Taskbar pictures of the Mizu theme for the acrylic taskbar.

dwm.exe puts acrylic behind the taskbar, so its pictures carry alpha:
  - TASKBANDBUTTON: the six states of a task button, as Windows 10 draws
    them on a dark taskbar: white at 0, 10, 6, 0, 15 and 22% (normal, hot,
    pressed, disabled, checked = the active window, hot checked), each with
    the 2 px line of a running program at the bottom in the accent's light
    shade;
  - STARTBUTTON: the Start button's three states: the Arctic snowflake over
    nothing, white at 10% and at 6%.

Usage: taskbar-bitmaps.py <project root> <out dir> [preview.png]
"""
import struct
import sys

from PIL import Image

LINE = (0x76, 0xB9, 0xED)  # the light shade of the accent (#0078D7)


def save_bmp(path, img):
    """32-bit BI_RGB, bottom-up, straight alpha: what uxtheme premultiplies itself."""
    img = img.convert("RGBA")
    w, h = img.size
    px = img.load()
    rows = bytearray()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = px[x, y]
            rows += bytes((b, g, r, a))
    header = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, len(rows), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(b"BM" + struct.pack("<IHHI", 14 + len(header) + len(rows), 0, 0, 14 + len(header)))
        f.write(header)
        f.write(rows)


def task_buttons():
    w, h = 26, 24
    img = Image.new("RGBA", (w, h * 6), (0, 0, 0, 0))
    px = img.load()
    for state, white in enumerate((0, 0.10, 0.06, 0, 0.15, 0.22)):
        for y in range(h):
            for x in range(w):
                if y >= h - 2 and state != 3:
                    px[x, state * h + y] = LINE + (255,)
                elif white:
                    px[x, state * h + y] = (255, 255, 255, round(255 * white))
    return img


def arctic_logo(path, size):
    """The snowflake of ARCTIC.png (white on black, above the name), with its
    brightness as alpha."""
    src = Image.open(path).convert("L")
    w, h = src.size
    top = src.crop((0, 0, w, h * 7 // 10))  # the name lies below the flake
    box = top.point(lambda v: 255 if v > 60 else 0).getbbox()
    flake = top.crop(box)
    side = max(flake.size)
    square = Image.new("L", (side, side), 0)
    square.paste(flake, ((side - flake.width) // 2, (side - flake.height) // 2))
    small = square.resize((size, size), Image.LANCZOS)
    # the flake's soft glow would be a grey square this small
    return small.point(lambda v: max(0, min(255, (v - 48) * 255 // 160)))


def start_button(logo_path):
    """Three states, 43x26 as the old picture: the Arctic snowflake over
    nothing, over white at 10% and at 6%."""
    w, h = 43, 26
    logo = arctic_logo(logo_path, 20).load()
    img = Image.new("RGBA", (w, h * 3), (0, 0, 0, 0))
    px = img.load()
    for state, white in enumerate((0, 0.10, 0.06)):
        for y in range(h):
            for x in range(w):
                lx, ly = x - (w - 20) // 2, y - (h - 20) // 2
                a = logo[lx, ly] / 255 if 0 <= lx < 20 and 0 <= ly < 20 else 0
                # the flake over the state's white, both straight alpha
                out_a = a + white * (1 - a)
                px[x, state * h + y] = (255, 255, 255, round(255 * out_a)) if out_a else (0, 0, 0, 0)
    return img


def main():
    root, out = sys.argv[1], sys.argv[2]
    buttons, start = task_buttons(), start_button(root + "/ARCTIC.png")
    save_bmp(out + "/NORMAL_TASKBANDBUTTON.bmp", buttons)
    save_bmp(out + "/NORMAL_STARTBUTTON.bmp", start)
    if len(sys.argv) > 3:
        # how they look over a dark acrylic
        preview = Image.new("RGBA", (300, 600), (32, 36, 44, 255))
        preview.alpha_composite(start.resize((start.width * 4, start.height * 4), Image.NEAREST), (0, 0))
        preview.alpha_composite(buttons.resize((buttons.width * 4, buttons.height * 4), Image.NEAREST), (190, 0))
        preview.save(sys.argv[3])


if __name__ == "__main__":
    main()
