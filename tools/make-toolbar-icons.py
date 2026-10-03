"""The Explorer toolbar's image strips (browseui 214-223) with Longhorn icons.

browseui draws its toolbar from strips of 47 icons in the order of Windows
XP's (back, forward, cut, copy, ... up, folders). The icons Explorer shows
come from the Longhorn icon theme (runtime/art/longhorn, PNG at 16, 22, 32
and 48 pixels); the others stay ReactOS's (runtime/art/browseui, the
original 24 pixel strips). One strip a size, as Windows picks the toolbar
size by the scale: 16, 24 (100 %), 30 (125 %), 36 (150 %), 48 (175 % and
up), each with its hot twin. 32 bits, straight alpha, bottom-up, as
browseui's own.

usage: python tools/make-toolbar-icons.py   (needs Pillow; writes
runtime/reactos/files/dll/win32/browseui/res/2xx.bmp)
"""
import os
import struct

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(ROOT, "runtime", "art")
OUT = os.path.join(ROOT, "runtime", "reactos", "files", "dll", "win32", "browseui", "res")
COUNT = 47

# strip index -> Longhorn icon (under <size>x<size>/)
LONGHORN = {
    0: "actions/back",
    1: "actions/forward",
    3: "actions/stock_add-bookmark",
    5: "actions/edit-cut",
    6: "actions/edit-copy",
    7: "actions/edit-paste",
    8: "actions/edit-undo",
    9: "actions/edit-redo",
    10: "actions/edit-delete",
    11: "actions/document-new",
    12: "actions/document-open",
    14: "actions/document-print-preview",
    15: "actions/document-properties",
    16: "actions/help-contents",
    17: "actions/edit-find",
    18: "actions/edit-find",
    19: "actions/document-print",
    28: "actions/go-up",
    31: "actions/folder-new",
    43: "places/folder",
}
SOURCE_SIZES = (16, 22, 32, 48)

# (pixels, normal strip, hot strip)
STRIPS = ((16, 216, 217), (24, 214, 215), (30, 218, 219), (36, 220, 221), (48, 222, 223))


def resized(image, size):
    """smoothly, on premultiplied colours, so edges keep no dark fringe"""
    if image.size == (size, size):
        return image
    return image.convert("RGBa").resize((size, size), Image.LANCZOS).convert("RGBA")


def longhorn(name, size):
    """the theme's own size if it has it, else the next larger one scaled down"""
    for source in SOURCE_SIZES:
        if source >= size:
            path = os.path.join(ART, "longhorn", "%dx%d" % (source, source), name + ".png")
            if os.path.exists(path):
                return resized(Image.open(path).convert("RGBA"), size)
    path = os.path.join(ART, "longhorn", "48x48", name + ".png")
    return resized(Image.open(path).convert("RGBA"), size)


def read_strip(path):
    """a browseui strip: 32-bit BI_RGB, bottom-up, straight alpha"""
    data = open(path, "rb").read()
    offset = struct.unpack("<I", data[10:14])[0]
    width, height = struct.unpack("<ii", data[18:26])
    image = Image.frombuffer("RGBA", (width, abs(height)), data[offset:offset + width * abs(height) * 4],
                             "raw", "BGRA", 0, -1 if height > 0 else 1)
    return image.copy()


def write_strip(image, path):
    width, height = image.size
    pixels = image.transpose(Image.FLIP_TOP_BOTTOM).tobytes("raw", "BGRA")
    header = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 32, 0, len(pixels), 2835, 2835, 0, 0)
    with open(path, "wb") as f:
        f.write(header + info + pixels)


def hot(icon):
    """the hot twin a shade brighter, as the button under the pointer"""
    r, g, b, a = icon.split()
    lift = lambda v: min(255, int(v * 1.08 + 6))
    return Image.merge("RGBA", (r.point(lift), g.point(lift), b.point(lift), a))


def main():
    base = {False: read_strip(os.path.join(ART, "browseui", "214.bmp")),
            True: read_strip(os.path.join(ART, "browseui", "215.bmp"))}
    os.makedirs(OUT, exist_ok=True)
    for size, normal_id, hot_id in STRIPS:
        for is_hot, strip_id in ((False, normal_id), (True, hot_id)):
            strip = Image.new("RGBA", (size * COUNT, size), (0, 0, 0, 0))
            for i in range(COUNT):
                if i in LONGHORN:
                    icon = longhorn(LONGHORN[i], size)
                    if is_hot:
                        icon = hot(icon)
                else:
                    icon = resized(base[is_hot].crop((i * 24, 0, i * 24 + 24, 24)), size)
                strip.paste(icon, (i * size, 0))
            write_strip(strip, os.path.join(OUT, "%d.bmp" % strip_id))
            print("%d.bmp: %d icons of %d pixels" % (strip_id, COUNT, size))


if __name__ == "__main__":
    main()
