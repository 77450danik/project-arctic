"""Bluetooth's icon for fsquirt.exe and the Send to link: the rune in white
on a blue rounded square, drawn at 4x and scaled down, in the sizes Windows
asks for (16 to 256). Usage: bluetooth-icon.py OUT.ico"""

import sys

from PIL import Image, ImageDraw

BLUE = (0, 120, 212, 255)
WHITE = (255, 255, 255, 255)


def frame(size):
    big = size * 4
    image = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    margin = big * 0.06
    draw.rounded_rectangle((margin, margin, big - margin, big - margin), radius=big * 0.22, fill=BLUE)
    # the rune on a 16 unit grid, as devicescpl's glyph
    unit = big / 16.0
    points = [(5.2, 4.8), (10.8, 10.2), (8, 13), (8, 3), (10.8, 5.8), (5.2, 11.2)]
    width = max(int(unit * 1.25), 2)
    line = [(x * unit, y * unit) for x, y in points]
    draw.line(line, fill=WHITE, width=width, joint="curve")
    for x, y in line:
        r = width / 2
        draw.ellipse((x - r, y - r, x + r, y + r), fill=WHITE)
    return image.resize((size, size), Image.LANCZOS)


def main():
    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    frames = [frame(s) for s in sizes]
    frames[-1].save(sys.argv[1], format="ICO", sizes=[(s, s) for s in sizes], append_images=frames[:-1])


if __name__ == "__main__":
    main()
