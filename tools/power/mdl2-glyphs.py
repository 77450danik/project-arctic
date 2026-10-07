#!/usr/bin/env python3
"""The battery glyphs of Segoe MDL2 Assets, as outlines for batmeter.dll.

mdl2-glyphs.py segmdl2.ttf OUT.h

Windows 10 draws the battery of the notification area and of its flyout
with glyphs of Segoe MDL2 Assets (Battery0-10, BatteryCharging0-10,
BatterySaver0-10, BatteryUnknown; the flyout's slider has a battery and a
lightning bolt at its ends). The font is not shipped: these outlines are,
flattened to polygons in the font's units (2048 an em, y up), and batmeter
fills them itself (glyph.c). Needs fontTools (a venv in the WSL build distro).
"""
import sys

from fontTools.pens.basePen import BasePen
from fontTools.ttLib import TTFont

GLYPHS = (
    [("Battery%d" % i, cp) for i, cp in enumerate(list(range(0xE850, 0xE85A)) + [0xE83F])] +
    [("BatteryCharging%d" % i, cp) for i, cp in enumerate(list(range(0xE85A, 0xE863)) + [0xE83E, 0xEA93])] +
    [("BatterySaver%d" % i, cp) for i, cp in enumerate(list(range(0xE863, 0xE86C)) + [0xEA94, 0xEA95])] +
    [("BatteryUnknown", 0xE996), ("LightningBolt", 0xE945)]
)
STEPS = 8  # segments a quadratic curve becomes


class Flatten(BasePen):
    def __init__(self, glyphset):
        super().__init__(glyphset)
        self.contours = []
        self.current = None

    def _moveTo(self, pt):
        self.current = [pt]

    def _lineTo(self, pt):
        self.current.append(pt)

    def _qCurveToOne(self, pt1, pt2):
        x0, y0 = self.current[-1]
        for i in range(1, STEPS + 1):
            t = i / STEPS
            x = (1 - t) ** 2 * x0 + 2 * (1 - t) * t * pt1[0] + t * t * pt2[0]
            y = (1 - t) ** 2 * y0 + 2 * (1 - t) * t * pt1[1] + t * t * pt2[1]
            self.current.append((x, y))

    def _curveToOne(self, pt1, pt2, pt3):
        x0, y0 = self.current[-1]
        for i in range(1, STEPS + 1):
            t = i / STEPS
            u = 1 - t
            x = u ** 3 * x0 + 3 * u * u * t * pt1[0] + 3 * u * t * t * pt2[0] + t ** 3 * pt3[0]
            y = u ** 3 * y0 + 3 * u * u * t * pt1[1] + 3 * u * t * t * pt2[1] + t ** 3 * pt3[1]
            self.current.append((x, y))

    def _closePath(self):
        if self.current:
            if len(self.current) > 1 and self.current[0] == self.current[-1]:
                self.current.pop()
            self.contours.append(self.current)
        self.current = None

    _endPath = _closePath


def main():
    font = TTFont(sys.argv[1])
    cmap = font.getBestCmap()
    glyphset = font.getGlyphSet()
    out = ["/*",
           " * Battery glyphs of Segoe MDL2 Assets as polygons (tools/power/mdl2-glyphs.py):",
           " * per glyph, each contour as its point count then x, y pairs in font units",
           " * (2048 an em, y up), ending with 0",
           " */",
           ""]
    table = []
    for name, cp in GLYPHS:
        pen = Flatten(glyphset)
        glyphset[cmap[cp]].draw(pen)
        values = []
        for contour in pen.contours:
            values.append(len(contour))
            for x, y in contour:
                values += [round(x), round(y)]
        values.append(0)
        out.append("static const short glyph_%04X[] = /* %s */" % (cp, name))
        out.append("{")
        for i in range(0, len(values), 16):
            out.append("    " + ", ".join(str(v) for v in values[i:i + 16]) + ",")
        out.append("};")
        out.append("")
        table.append((cp, name))
    out.append("static const struct { WCHAR code; const short *outline; } glyphs[] =")
    out.append("{")
    for cp, name in table:
        out.append("    { 0x%04X, glyph_%04X },  /* %s */" % (cp, cp, name))
    out.append("};")
    with open(sys.argv[2], "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
