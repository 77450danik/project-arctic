"""Renders the glyph atlas the stop screen (host/init/stop.c) draws text with.

usage: mkfont.py <font.ttf> <out.font>

Sizes are for a 3840x2160 screen; smaller screens scale them down.
File layout (little endian):
  "ARFN", u32 version, u32 set count
  per set: u32 id, u32 line height, u32 ascent, u32 glyph count,
           glyphs sorted by code point: u32 cp, i16 left, i16 top, u16 w, u16 h, u16 advance, u32 offset
  u32 bitmap size, 8-bit alpha bitmaps
"""
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

UKRAINIAN = "АБВГҐДЕЄЖЗИІЇЙКЛМНОПРСТУФХЦЧШЩЬЮЯабвгґдеєжзиіїйклмнопрстуфхцчшщьюя"
TEXT = "".join(chr(c) for c in range(32, 127)) + UKRAINIAN + "’ʼ«»–—№"
SETS = [
    (0, 400, ":("),   # the sad face
    (1, 60, TEXT),    # main message and progress
    (2, 34, TEXT),    # stop code lines
]

font_path, out_path = sys.argv[1], sys.argv[2]
tables, bitmaps = [], bytearray()
for set_id, size, chars in SETS:
    font = ImageFont.truetype(font_path, size)
    ascent, descent = font.getmetrics()
    glyphs = []
    for ch in sorted(set(chars)):
        x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        offset = len(bitmaps)
        if w and h:
            image = Image.new("L", (w, h), 0)
            ImageDraw.Draw(image).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
            bitmaps += image.tobytes()
        glyphs.append(struct.pack("<IhhHHHI", ord(ch), x0, y0, w, h, round(font.getlength(ch)), offset))
    tables.append(struct.pack("<IIII", set_id, ascent + descent, ascent, len(glyphs)) + b"".join(glyphs))

with open(out_path, "wb") as out:
    out.write(b"ARFN" + struct.pack("<II", 1, len(SETS)))
    for table in tables:
        out.write(table)
    out.write(struct.pack("<I", len(bitmaps)))
    out.write(bitmaps)
