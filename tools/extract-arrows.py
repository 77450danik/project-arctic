"""Explorer's back, forward and up arrows from the drawing they were made in.

The drawing is one JPEG on white: back and forward side by side on top, up
below, each with a soft grey shadow. Everything grey or white that reaches
the border is background (the shadows too); what is left is an arrow, cut
out at full size and drawn into a square, all three at one scale, so that
make-toolbar-icons.py scales them down to the toolbar's sizes with smooth
edges.

usage: python tools/extract-arrows.py <drawing.jpeg>
       (needs Pillow and numpy; writes runtime/art/arrows/{back,forward,up}.png)
"""
import os
import sys

import numpy
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "runtime", "art", "arrows")
SIZE = 256


def background(rgb):
    """grey and white connected to the border: the paper and the shadows"""
    pixels = rgb.astype(numpy.int16)
    chroma = pixels.max(axis=2) - pixels.min(axis=2)
    grey = Image.fromarray(numpy.where(chroma < 34, 255, 0).astype(numpy.uint8))
    # flood the grey from a frame of seeds round the edge
    height, width = grey.height, grey.width
    framed = Image.new("L", (width + 2, height + 2), 255)
    framed.paste(grey, (1, 1))
    ImageDraw.floodfill(framed, (0, 0), 128, thresh=0)
    flooded = numpy.asarray(framed)[1:-1, 1:-1]
    return flooded == 128


def cut(rgb, paper, box):
    left, top, right, bottom = box
    mask = ~paper[top:bottom, left:right]
    ys, xs = numpy.nonzero(mask)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    alpha = Image.fromarray((mask[y0:y1, x0:x1] * 255).astype(numpy.uint8))
    # the outermost pixel or two are the arrow's rim blended with the paper
    alpha = alpha.filter(ImageFilter.MinFilter(3))
    colour = Image.fromarray(rgb[top + y0:top + y1, left + x0:left + x1])
    arrow = colour.convert("RGBA")
    arrow.putalpha(alpha)
    return arrow


def main():
    drawing = Image.open(sys.argv[1]).convert("RGB")
    rgb = numpy.asarray(drawing)
    paper = background(rgb)
    width, height = drawing.size
    split_x, split_y = width // 2, int(height * 0.48)
    arrows = {
        "back": cut(rgb, paper, (0, 0, split_x, split_y)),
        "forward": cut(rgb, paper, (split_x, 0, width, split_y)),
        "up": cut(rgb, paper, (0, split_y, width, height)),
    }
    # one scale for all three: the largest side fills the square
    side = int(max(max(a.size) for a in arrows.values()) * 1.01)
    os.makedirs(OUT, exist_ok=True)
    for name, arrow in arrows.items():
        square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        square.paste(arrow, ((side - arrow.width) // 2, (side - arrow.height) // 2))
        small = square.convert("RGBa").resize((SIZE, SIZE), Image.LANCZOS).convert("RGBA")
        small.save(os.path.join(OUT, name + ".png"))
        print("%s.png: %dx%d from %dx%d" % (name, SIZE, SIZE, arrow.width, arrow.height))


if __name__ == "__main__":
    main()
