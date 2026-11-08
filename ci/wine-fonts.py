#!/usr/bin/env python3
"""Wine's own fonts as fonts of C:\\Windows\\Fonts.

Wine ships Tahoma, Marlett, Symbol, Webdings, Wingdings and the bitmap fonts
of Windows (MS Sans Serif, Courier, Small Fonts, System...) in its data
directory, and registers them under Fonts with their Unix paths
(\\??\\unix\\usr\\share\\wine\\fonts\\tahoma.ttf). In Arctic they are files of
C:\\Windows\\Fonts, as in Windows, registered by their names
(runtime/registry/fonts.reg), and the data directory has none left.

The glyphs stay as they are. What says "Wine" in the TrueType name tables
goes: the PostScript and full names ("WineTahoma", "Wine Tahoma Regular")
are the family's, the maker "Wine" and the address of its site are dropped.
The authors' copyright and the license (LGPL) are kept, with "Wine" taken out
of a font's own name in them ("WineSymbol (c) ..." is "Symbol (c) ...").
The bitmap fonts (.fon) carry no such name and are copied as they are.

usage: wine-fonts.py WINE_FONTS_DIR FONTS_DIR
"""
import os
import re
import shutil
import sys

from fontTools.ttLib import TTFont

DROP = {8, 11}          # manufacturer, vendor URL
WINE_WORD = re.compile(r"\bWine ?(?=[A-Z])|\bWine\b ?")


def clean(text):
    return WINE_WORD.sub("", text).strip()


def clean_ttf(source, target):
    font = TTFont(source, recalcTimestamp=False)
    table = font["name"]
    family = None
    for record in table.names:
        if record.nameID == 1:
            family = clean(record.toUnicode())
            break
    for record in list(table.names):
        if record.nameID in DROP:
            table.removeNames(nameID=record.nameID, platformID=record.platformID,
                              platEncID=record.platEncID, langID=record.langID)
            continue
        text = record.toUnicode()
        if "Wine" not in text and "winehq" not in text.lower():
            continue
        if record.nameID == 6 and family:          # PostScript: no spaces
            new = family.replace(" ", "") + ("-Bold" if "bold" in text.lower() else "")
        elif record.nameID == 4 and family:        # the full name
            new = clean(text)
            new = re.sub(r"\s+Regular$", "", new)
        else:
            new = clean(text)
        record.string = new
    font.save(target)
    left = [r.toUnicode() for r in font["name"].names if "wine" in r.toUnicode().lower()]
    if left:
        raise SystemExit("%s still says Wine: %r" % (source, left))


def main():
    source_dir, fonts_dir = sys.argv[1], sys.argv[2]
    os.makedirs(fonts_dir, exist_ok=True)
    count = 0
    for name in sorted(os.listdir(source_dir)):
        source = os.path.join(source_dir, name)
        target = os.path.join(fonts_dir, name)
        if name.lower().endswith(".ttf"):
            clean_ttf(source, target)
        elif name.lower().endswith(".fon"):
            shutil.copyfile(source, target)
        else:
            continue
        count += 1
    print("%d fonts of Wine in %s" % (count, fonts_dir))


if __name__ == "__main__":
    main()
