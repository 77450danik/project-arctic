#!/usr/bin/env python3
"""Mizu's dark colour scheme, made from its light one.

The visual style of Arctic is ReactOS's Mizu: white and light grey parts with
blue accents. Its "Dark" colour scheme (runtime/reactos/files/media/themes/
Mizu, ReactOS patch "mizu-the-dark-colour-scheme") has the same parts, sizes
and margins, made dark here:

- every picture of Normal\\*.bmp as Dark\\*.bmp, its lightness turned over in
  HLS (white is #2b2b2b, black #e6e6e6), hue and saturation kept, so blue
  stays blue and grows lighter, as Windows 11's accent does in dark mode;
  the colours that stand for transparency (magenta, and the red or black
  some parts name as TransparentColor) and the alpha stay as they are;
- every colour of the INI files the same way, except the system colours
  ([SysMetrics]), which are Windows 11's dark ones (startui's
  personalize.c applies the same values with SetSysColors).

usage: make-dark-mizu.py MIZU_SOURCE_DIR OUT_DIR
  MIZU_SOURCE_DIR  ReactOS's media/themes/Mizu/mizu.msstyles
  OUT_DIR          runtime/reactos/files/media/themes/Mizu/mizu.msstyles
"""
import colorsys
import os
import re
import sys

from PIL import Image

MAGENTA = (255, 0, 255)

# Windows 11's dark system colours, as personalize.c sets them
SYS_METRICS = {
    "ActiveCaption": "32 32 32",
    "Btnface": "32 32 32",
    "BtnHighlight": "70 70 70",
    "BtnShadow": "90 90 90",
    "CaptionText": "255 255 255",
    "DkShadow3d": "20 20 20",
    "GradientActiveCaption": "32 32 32",
    "GradientInactiveCaption": "32 32 32",
    "GrayText": "128 128 128",
    "Highlight": "65 65 65",
    "HighlightText": "255 255 255",
    "HotTracking": "153 214 255",
    "InactiveCaption": "32 32 32",
    "InactiveCaptionText": "128 128 128",
    "Light3d": "60 60 60",
    "Menu": "43 43 43",
    "MenuBar": "32 32 32",
    "MenuHilight": "65 65 65",
    "MenuText": "255 255 255",
    "Window": "25 25 25",
}


def dark(rgb, keep=()):
    """A colour of the light scheme in the dark one; the see-through ones stay."""
    if tuple(rgb) == MAGENTA or tuple(rgb) in keep:
        return tuple(rgb)
    r, g, b = (c / 255.0 for c in rgb)
    h, l, s = colorsys.rgb_to_hls(r, g, b)
    l = 0.17 + (1.0 - l) * 0.73
    r, g, b = colorsys.hls_to_rgb(h, l, s)
    return tuple(max(0, min(255, int(round(c * 255)))) for c in (r, g, b))


def dark_bitmap(source, target, keep=()):
    im = Image.open(source)
    has_alpha = im.mode == "RGBA"
    rgba = im.convert("RGBA")
    pixels = rgba.load()
    cache = {}
    for y in range(rgba.size[1]):
        for x in range(rgba.size[0]):
            r, g, b, a = pixels[x, y]
            key = (r, g, b)
            if key not in cache:
                cache[key] = dark(key, keep)
            pixels[x, y] = cache[key] + (a,)
    if has_alpha:
        # 32-bit as the source: BI_RGB with its alpha, which Pillow writes as is
        rgba.save(target, "BMP")
    else:
        rgba.convert("RGB").save(target, "BMP")


COLOR = re.compile(r"^(\s*[A-Za-z0-9]+\s*=\s*)(\d{1,3})\s+(\d{1,3})\s+(\d{1,3})(\s*(;.*)?)$")

# The taskbar, the Start menus and the clock take the look of the system's
# mode (startui's taskbar look), not the programs': their parts stay light.
SHELL_CLASSES = ("taskbar", "taskband", "startpanel", "startmenu", "start::", "clock", "traynotify", "tray",
                 "menuband", "toolbar.startbutton")


def shell_section(section):
    return any(name in section for name in SHELL_CLASSES)


def dark_ini(source, target):
    out = []
    section = ""
    with open(source, encoding="utf-8-sig", errors="replace") as f:
        lines = f.read().splitlines()
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1].lower()
            out.append(line)
            continue
        if shell_section(section):
            out.append(line)
            continue
        line = line.replace("Normal\\", "Dark\\")
        if section == "sysmetrics":
            key = stripped.split("=", 1)[0].strip()
            for name, value in SYS_METRICS.items():
                if key.lower() == name.lower():
                    line = "%s = %s" % (key, value)
                    break
            out.append(line)
            continue
        m = COLOR.match(line)
        if m and "transparent" not in line.lower():
            rgb = dark((int(m.group(2)), int(m.group(3)), int(m.group(4))))
            line = "%s%d %d %d%s" % (m.group(1), rgb[0], rgb[1], rgb[2], m.group(5) or "")
        out.append(line)
    with open(target, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(out) + "\n")


IMAGE = re.compile(r"^\s*\w*imagefile\d*\s*=\s*Normal\\([A-Za-z0-9_-]+)\.bmp", re.I)
TRANSPARENT = re.compile(r"^\s*\w*transparentcolor\s*=\s*(\d{1,3})\s+(\d{1,3})\s+(\d{1,3})", re.I)


def see_through_colours(source, keep):
    """The colours each picture shows the background with (TransparentColor
    of the parts that name it: magenta, but also red and black in Mizu),
    which must stay as they are."""
    with open(source, encoding="utf-8-sig", errors="replace") as f:
        sections = re.split(r"(?m)^\s*\[", f.read())
    for section in sections:
        names = set()
        colours = set()
        for line in section.splitlines():
            m = IMAGE.match(line)
            if m:
                names.add(m.group(1).upper())
            m = TRANSPARENT.match(line)
            if m:
                colours.add(tuple(int(c) for c in m.groups()))
        for name in names:
            keep.setdefault(name, set()).update(colours)


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.join(out, "bitmaps"), exist_ok=True)
    os.makedirs(os.path.join(out, "textfiles"), exist_ok=True)
    referenced = set()
    keep = {}
    for size in ("Normal", "LargeFonts", "ExtraLarge"):
        target = os.path.join(out, "textfiles", "%sDark.INI" % size)
        see_through_colours(os.path.join(src, "textfiles", "%sNormal.INI" % size), keep)
        dark_ini(os.path.join(src, "textfiles", "%sNormal.INI" % size), target)
        with open(target, encoding="utf-8") as f:
            referenced |= {m.upper() for m in re.findall(r"Dark\\([A-Za-z0-9_-]+)\.bmp", f.read(), re.I)}
    # the pictures the dark parts name; Arctic's own pictures of some parts
    # stand in for ReactOS's
    overrides = os.path.join(out, "bitmaps")
    for name in sorted(referenced):
        source = os.path.join(overrides, "NORMAL_%s.bmp" % name)
        if not os.path.exists(source):
            source = os.path.join(src, "bitmaps", "NORMAL_%s.bmp" % name)
        if not os.path.exists(source):
            # Mizu names a few pictures it does not have (ExplorerBarHeaderPin)
            referenced.discard(name)
            continue
        dark_bitmap(source, os.path.join(out, "bitmaps", "DARK_%s.bmp" % name), keep.get(name, ()))
    # mizu.rc names each of them (ReactOS patch "mizu-the-dark-colour-scheme")
    print("%d pictures and 3 INI files of the dark scheme in %s" % (len(referenced), out))


if __name__ == "__main__":
    main()
