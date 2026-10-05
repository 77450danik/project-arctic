"""Draws the icon of the Snipping Tool (runtime/wine/modules/programs/snippingtool):
a blue rounded tile with the white corners of a crop frame and a red dot, the
point marked on the snip. Arctic's own drawing, in the sizes Explorer asks for.

usage: python tools/draw-snippingtool-icon.py
"""
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "runtime", "wine", "modules", "programs", "snippingtool", "snippingtool.ico")
SIZES = (16, 24, 32, 48, 64, 256)
SS = 4  # supersampling


def tile(size):
    big = size * SS
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))

    # the tile: a vertical gradient, light blue at the top
    gradient = Image.new("RGBA", (big, big))
    top, bottom = (76, 194, 255), (0, 95, 184)
    for y in range(big):
        t = y / max(1, big - 1)
        gradient.paste(tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,), (0, y, big, y + 1))
    mask = Image.new("L", (big, big), 0)
    inset = big * 0.06
    ImageDraw.Draw(mask).rounded_rectangle((inset, inset, big - inset, big - inset), radius=big * 0.22, fill=255)
    img.paste(gradient, (0, 0), mask)

    # the crop frame's corners
    draw = ImageDraw.Draw(img)
    width = max(SS, round(big * (0.1 if size <= 24 else 0.075)))
    a, b, arm = big * 0.25, big * 0.75, big * 0.17
    for (x, y, dx, dy) in ((a, a, 1, 1), (b, a, -1, 1), (a, b, 1, -1), (b, b, -1, -1)):
        draw.line([(x + dx * arm, y), (x, y), (x, y + dy * arm)], fill=(255, 255, 255, 255), width=width,
                  joint="curve")
        for px, py in ((x + dx * arm, y), (x, y + dy * arm), (x, y)):
            r = width / 2
            draw.ellipse((px - r, py - r, px + r, py + r), fill=(255, 255, 255, 255))

    # the marked point
    r = big * (0.11 if size <= 24 else 0.09)
    c = big / 2
    draw.ellipse((c - r, c - r, c + r, c + r), fill=(255, 92, 92, 255))
    return img.resize((size, size), Image.LANCZOS)


def main():
    images = [tile(s) for s in SIZES]
    images[-1].save(OUT, format="ICO", sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    print("wrote", OUT)


if __name__ == "__main__":
    main()
