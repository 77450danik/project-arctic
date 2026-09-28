#!/usr/bin/env python3
"""The core fonts of Windows under their own names, made from Liberation.

Programs ask for fonts by name, and Chromium (Chrome, Edge, Electron, the
Steam client and its overlay) takes fonts only from Windows\\Fonts, so with
no Arial there it draws no text at all. Liberation Sans, Serif and Mono have
the metrics of Arial, Times New Roman and Courier New and cover Cyrillic;
Proton gives them the same names. The SIL Open Font License lets a modified
font be shared under a name other than its Reserved Font Name, so the
copyright and license records are kept and the Liberation name goes.

usage: windows-fonts.py LIBERATION_DIR FONTS_DIR
"""
import os
import sys

from fontTools.ttLib import TTFont

FAMILIES = {
    "LiberationSans": ("Arial", "arial", "Arial"),
    "LiberationSerif": ("Times New Roman", "times", "TimesNewRomanPS"),
    "LiberationMono": ("Courier New", "cour", "CourierNewPS"),
}
STYLES = {
    "Regular": ("Regular", "", ""),
    "Bold": ("Bold", "bd", "-Bold"),
    "Italic": ("Italic", "i", "-Italic"),
    "BoldItalic": ("Bold Italic", "bi", "-BoldItalic"),
}


def rename(source, target, family, style, postscript):
    font = TTFont(source)
    full = family if style == "Regular" else "%s %s" % (family, style)
    names = {
        1: family,
        2: style,
        3: "Arctic: %s" % full,
        4: full,
        6: postscript,
    }
    table = font["name"]
    for record in list(table.names):
        if record.nameID in names:
            record.string = names[record.nameID]
        elif record.nameID in (7, 16, 17, 21, 22):  # trademark, typographic names
            table.removeNames(nameID=record.nameID)
    font.save(target)


def main():
    source_dir, fonts_dir = sys.argv[1], sys.argv[2]
    for base, (family, file_prefix, postscript) in FAMILIES.items():
        for style_key, (style, file_suffix, ps_suffix) in STYLES.items():
            source = os.path.join(source_dir, "%s-%s.ttf" % (base, style_key))
            target = os.path.join(fonts_dir, "%s%s.ttf" % (file_prefix, file_suffix))
            rename(source, target, family, style, postscript + ps_suffix + "MT")
            print("%s -> %s (%s %s)" % (os.path.basename(source), os.path.basename(target), family, style))


if __name__ == "__main__":
    main()
