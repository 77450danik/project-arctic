"""Converts the boot logo PNG into the raw format host/init/bootanim.c reads.

The logo's glow does not quite reach black at the edges of the picture
(up to RGB 5,7,8), which on the black boot screen showed as a faint box;
the last FADE pixels to each edge fade to black.
"""
import struct
import sys

from PIL import Image, ImageChops

FADE = 24

image = Image.open(sys.argv[1]).convert("RGB")
width, height = image.size
mask = Image.new("L", (width, height))
mask.putdata([round(255 * min(1, x / FADE, y / FADE, (width - 1 - x) / FADE, (height - 1 - y) / FADE))
              for y in range(height) for x in range(width)])
image = ImageChops.multiply(image, Image.merge("RGB", (mask, mask, mask)))
with open(sys.argv[2], "wb") as out:
    out.write(b"ARLG" + struct.pack("<II", width, height))
    out.write(image.tobytes("raw", "BGRX"))
