"""Renders Windows 11's boot spinner for the boot screen (host/init/bootanim.c).

usage: mkspinner.py <segoe_slboot_ex.ttf> <out>

Windows draws it from its boot font: glyphs U+E100..U+E176 of Segoe Boot
Semilight (segoe_slboot_ex.ttf, Windows 11) are its 119 frames, played at 60
a second. (U+E052..U+E0CB are Windows 8 and 10's ring of dots.) The frames
are rendered here at a range of sizes, so the boot screen picks the one for
its resolution rather than scaling one.

File layout (little endian):
  "ARSP", u32 frame count, u32 size count
  per size: u32 side, then each frame as side * side 8-bit alpha
"""
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

FIRST, COUNT = 0xE100, 119
# the side of a frame: the ring is about a twentieth of the screen's height
# across (bootanim.c), so 720p to 4K and a little beyond
SIDES = [28, 32, 36, 40, 44, 48, 54, 60, 66, 72, 80, 90, 100, 110, 124]

font_path, out_path = sys.argv[1], sys.argv[2]
with open(out_path, "wb") as out:
    out.write(b"ARSP" + struct.pack("<II", COUNT, len(SIDES)))
    for side in SIDES:
        # the glyphs span 4/3 of the font size across, centred on "mm"
        font = ImageFont.truetype(font_path, round(side * 3 / 4))
        out.write(struct.pack("<I", side))
        for n in range(COUNT):
            frame = Image.new("L", (side, side), 0)
            ImageDraw.Draw(frame).text((side / 2, side / 2), chr(FIRST + n), font=font, fill=255, anchor="mm")
            out.write(frame.tobytes())
