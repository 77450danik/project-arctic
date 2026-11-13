#!/usr/bin/env python3
"""The pictures of Arctic WordPad's ribbon, title bar and File page.

They are drawn as the pictures of the shell's menus are (tools/menu-icons.py,
whose designs and roles this takes): a 16-unit design of lines one unit wide,
grey lines, blue where the command acts, white inside closed shapes. Those
the menus have already (Cut, Copy, Paste, Undo, Redo, Print, Search, Open)
are taken from there, so the ribbon and the menus agree.

What a letter shows (Bold, Italic, Underline, the case, sub- and superscript)
is not drawn here: the program writes the letter in a font, as Word does.

Output: runtime/wine/modules/programs/wordpad/icons.bin, the "AMI1" format of
menu-icons.py, at the sizes the ribbon uses from 100 % to 200 %: 16 to 32 for
small buttons, 32 to 64 for large ones.

    tools/wordpad-icons.py                  writes the resource
    tools/wordpad-icons.py --sheet out.png  draws every picture for review
"""
import importlib.util
import math
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "runtime/wine/modules/programs/wordpad/icons.bin")

_spec = importlib.util.spec_from_file_location("menu_icons", os.path.join(ROOT, "tools/menu-icons.py"))
mi = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mi)

Path, line, poly, rrect, circle, rot = mi.Path, mi.line, mi.poly, mi.rrect, mi.circle, mi.rot
B, A, W, K, D, E = mi.B, mi.A, mi.W, mi.K, mi.D, mi.E
SIZES = (16, 20, 24, 28, 32, 40, 48, 56, 64)

ICONS = []
TAKEN = ("cut", "copy", "paste", "undo", "redo", "print", "search", "folder", "properties")


def icon(name):
    i = mi.Icon(name)
    ICONS.append(i)
    return i


def letter_a(ic, x0, y0, x1, y1, role=B, w=1.0):
    """a capital A standing in the box"""
    cx = (x0 + x1) / 2
    ic.s(line((x0, y1), (cx, y0), (x1, y1)), role, w)
    t = 0.62
    ic.s(line((x0 + (cx - x0) * (1 - t) + 0.3, y0 + (y1 - y0) * t), (x1 - (x1 - cx) * (1 - t) - 0.3, y0 + (y1 - y0) * t)), role, w)
    return ic


def page(ic, x0, y0, x1, y1, role=B, fill=W, fold=3.0):
    """a sheet with its corner folded"""
    ic.sf(Path().m(x0 + 1.5, y0).l(x1 - fold, y0).l(x1, y0 + fold).l(x1, y1 - 1.5)
          .arc(x1 - 1.5, y1 - 1.5, 1.5, 0, 90).l(x0 + 1.5, y1).arc(x0 + 1.5, y1 - 1.5, 1.5, 90, 180)
          .l(x0, y0 + 1.5).arc(x0 + 1.5, y0 + 1.5, 1.5, 180, 270).z(), role, fill)
    ic.s(line((x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)), role)
    return ic


def lines(ic, rows, role=B):
    for x0, y, x1 in rows:
        ic.s(line((x0, y), (x1, y)), role)
    return ic


def define():
    mi.define()
    for name in TAKEN:
        ICONS.append(next(ic for ic in mi.ICONS if ic.name == name))

    # Format Painter: the brush's head, its neck, the handle blue
    (icon("formatpainter")
     .sf(rrect(1.5, 1.5, 12.5, 6.5, 1.5), B)
     .s(line((12.5, 4.0), (14.5, 4.0), (14.5, 8.5), (8.0, 8.5), (8.0, 10.0)), B)
     .sf(rrect(6.5, 10.0, 9.5, 15.5, 1.25), A, A))

    # Grow and Shrink Font: an A and a caret
    letter_a(icon("growfont"), 1.0, 3.0, 10.0, 15.0).s(line((10.5, 6.0), (12.75, 3.0), (15.0, 6.0)), A)
    letter_a(icon("shrinkfont"), 1.0, 3.0, 10.0, 15.0).s(line((10.5, 3.0), (12.75, 6.0), (15.0, 3.0)), A)

    # Clear All Formatting: an A and an eraser across its foot
    ic = letter_a(icon("clearformat"), 1.0, 1.0, 10.0, 12.5)
    rubber = [(x + 11.0, y + 12.0) for x, y in rot([(-4.5, -1.75), (4.5, -1.75), (4.5, 1.75), (-4.5, 1.75)], 0, 0, -40)]
    ic.x(poly(*rubber), w=2.0)
    ic.sf(poly(*rubber), A)
    ic.s(line(*[(x + 11.0, y + 12.0) for x, y in rot([(0.0, -1.75), (0.0, 1.75)], 0, 0, -40)]), A)

    # Font Color: the A alone; the bar under it is the colour, drawn by the program
    letter_a(icon("fontcolor"), 3.0, 0.5, 13.0, 11.5)
    # Text Effects: an A with a blue glow
    ic = icon("texteffects")
    letter_a(ic, 2.5, 1.0, 13.5, 14.5, A, 2.6)
    ic.x(line((2.5, 14.5), (8.0, 1.0), (13.5, 14.5)), w=1.2, cap="flat")
    letter_a(ic, 2.5, 1.0, 13.5, 14.5, W, 0.01)
    # Text Highlight Color: a marker over a line
    marker = [(x + 9.0, y + 6.5) for x, y in rot([(-2.4, -6.0), (2.4, -6.0), (2.4, 2.5), (0.9, 5.0), (-0.9, 5.0), (-2.4, 2.5)], 0, 0, 40)]
    (icon("highlight")
     .sf(poly(*marker), B)
     .s(line(*[(x + 9.0, y + 6.5) for x, y in rot([(-2.4, 2.5), (2.4, 2.5)], 0, 0, 40)]), B)
     .f(poly(*[(x + 9.0, y + 6.5) for x, y in rot([(-0.9, 5.0), (0.9, 5.0), (0.9, 6.8), (-0.9, 6.8)], 0, 0, 40)]), A))

    # Bullets, Numbering, Multilevel List
    ic = icon("bullets")
    for y in (3.0, 8.0, 13.0):
        ic.f(circle(2.5, y, 1.5), A)
        ic.s(line((6.0, y), (15.5, y)), B)
    ic = icon("numbering")
    ic.s(line((1.6, 1.4), (2.6, 0.9), (2.6, 5.0)), A, 0.9)
    ic.s(Path().m(1.2, 7.2).arc(2.5, 7.4, 1.3, 200, 360).l(1.2, 10.5).l(3.9, 10.5), A, 0.9)
    ic.s(Path().m(1.2, 12.0).l(3.6, 12.0).l(2.3, 13.3).arc(2.4, 14.4, 1.2, 270, 450).l(1.1, 15.6), A, 0.9)
    for y in (3.0, 8.5, 14.0):
        ic.s(line((6.0, y), (15.5, y)), B)
    ic = icon("multilevel")
    for x, y in ((1.0, 2.5), (5.0, 8.0), (9.0, 13.5)):
        ic.f(rrect(x, y - 1.25, x + 2.5, y + 1.25, 0.5), A)
        ic.s(line((x + 4.5, y), (15.5, y)), B)

    # Decrease and Increase Indent
    ic = lines(icon("outdent"), [(0.5, 1.5, 15.5), (7.5, 6.0, 15.5), (7.5, 10.5, 15.5), (0.5, 14.5, 15.5)])
    ic.sf(poly((5.0, 5.0), (1.0, 8.25), (5.0, 11.5)), A, A)
    ic = lines(icon("indent"), [(0.5, 1.5, 15.5), (7.5, 6.0, 15.5), (7.5, 10.5, 15.5), (0.5, 14.5, 15.5)])
    ic.sf(poly((1.0, 5.0), (5.0, 8.25), (1.0, 11.5)), A, A)

    # Sort: A over Z, the arrow down
    ic = icon("sort")
    letter_a(ic, 0.75, 0.75, 6.75, 7.0, B, 1.0)
    ic.s(line((1.0, 9.5), (6.5, 9.5), (1.0, 14.75), (6.5, 14.75)), B)
    ic.s(line((11.5, 1.0), (11.5, 14.5)), A).s(line((8.5, 11.5), (11.5, 14.5), (14.5, 11.5)), A)

    # Show/Hide paragraph marks: the pilcrow
    (icon("pilcrow")
     .f(Path().m(9.0, 1.5).l(6.5, 1.5).arc(6.5, 5.25, 3.75, 270, 90).l(9.0, 9.0).z(), B)
     .s(line((9.0, 1.0), (9.0, 15.5)), B)
     .s(line((12.5, 1.0), (12.5, 15.5)), B)
     .s(line((6.5, 1.5), (14.5, 1.5)), B))

    # the alignments
    lines(icon("alignleft"), [(0.5, 2.5, 15.5), (0.5, 6.5, 10.5), (0.5, 10.5, 15.5), (0.5, 14.5, 10.5)])
    lines(icon("aligncenter"), [(0.5, 2.5, 15.5), (3.0, 6.5, 13.0), (0.5, 10.5, 15.5), (3.0, 14.5, 13.0)])
    lines(icon("alignright"), [(0.5, 2.5, 15.5), (5.5, 6.5, 15.5), (0.5, 10.5, 15.5), (5.5, 14.5, 15.5)])
    lines(icon("justify"), [(0.5, 2.5, 15.5), (0.5, 6.5, 15.5), (0.5, 10.5, 15.5), (0.5, 14.5, 9.5)])

    # Line and Paragraph Spacing: lines and the two arrows
    ic = lines(icon("linespacing"), [(7.0, 2.5, 15.5), (7.0, 6.5, 15.5), (7.0, 10.5, 15.5), (7.0, 14.5, 15.5)])
    ic.s(line((2.75, 1.5), (2.75, 14.5)), A).s(line((0.75, 3.5), (2.75, 1.5), (4.75, 3.5)), A).s(
        line((0.75, 12.5), (2.75, 14.5), (4.75, 12.5)), A)

    # Shading: the bucket tipped, its drop
    bucket = [(x + 7.0, y + 7.0) for x, y in rot([(-4.0, -4.0), (4.0, -4.0), (4.0, 3.0), (-4.0, 3.0)], 0, 0, 45)]
    (icon("shading")
     .sf(poly(*bucket), B)
     .s(line((3.5, 5.5), (3.5, 1.0)), B)
     .f(Path().m(13.5, 8.5).l(15.0, 11.0).arc(13.5, 11.5, 1.5, 345, 555).z(), A))

    # Borders: the dotted square, its bottom drawn
    ic = icon("borders")
    dots = set()
    for t in (0.5, 3.0, 5.5, 8.0, 10.5, 13.0):
        dots |= {(t, 0.5), (0.5, t), (15.5, t), (t, 8.0), (8.0, t)}
    dots.add((15.5, 0.5))
    for x, y in sorted(dots):
        ic.f(rrect(x - 0.6, y - 0.6, x + 0.6, y + 0.6, 0.0), B)
    ic.s(line((0.0, 15.0), (16.0, 15.0)), A, 1.5)

    # Replace: two arrows going round
    (icon("replace")
     .s(Path().m(2.0, 7.0).l(2.0, 5.5).arc(4.5, 5.5, 2.5, 180, 270).l(13.0, 3.0), B)
     .s(line((10.5, 0.75), (13.0, 3.0), (10.5, 5.25)), B)
     .s(Path().m(14.0, 9.0).l(14.0, 10.5).arc(11.5, 10.5, 2.5, 0, 90).l(3.0, 13.0), A)
     .s(line((5.5, 10.75), (3.0, 13.0), (5.5, 15.25)), A))

    # Select: the pointer
    icon("select").sf(poly((3.5, 0.75), (13.0, 10.0), (8.5, 10.25), (11.25, 15.0), (9.25, 15.75),
                           (6.75, 11.0), (3.5, 14.0)), B)

    # Page Break: the end of one page, the top of the next, the break blue
    (icon("pagebreak")
     .s(Path().m(2.5, 0.5).l(2.5, 4.0).arc(4.0, 4.0, 1.5, 180, 90).l(12.0, 5.5).arc(12.0, 4.0, 1.5, 90, 0).l(13.5, 0.5), B)
     .s(Path().m(2.5, 15.5).l(2.5, 12.0).arc(4.0, 12.0, 1.5, 180, 270).l(12.0, 10.5).arc(12.0, 12.0, 1.5, 270, 360).l(13.5, 15.5), B)
     .s(line((0.0, 8.0), (2.5, 8.0)), A).s(line((4.5, 8.0), (7.0, 8.0)), A)
     .s(line((9.0, 8.0), (11.5, 8.0)), A).s(line((13.5, 8.0), (16.0, 8.0)), A))

    # Table: the grid, its top row blue
    (icon("table")
     .f(rrect(0.5, 1.5, 15.5, 14.5, 1.5), W)
     .f(Path().m(0.5, 3.0).arc(2.0, 3.0, 1.5, 180, 270).l(14.0, 1.5).arc(14.0, 3.0, 1.5, 270, 360).l(15.5, 5.5).l(0.5, 5.5).z(), A)
     .s(rrect(0.5, 1.5, 15.5, 14.5, 1.5), B)
     .s(line((0.5, 5.5), (15.5, 5.5)), B).s(line((0.5, 10.0), (15.5, 10.0)), B)
     .s(line((5.5, 5.5), (5.5, 14.5)), B).s(line((10.5, 5.5), (10.5, 14.5)), B))

    # Pictures: a frame, the hills and the sun
    (icon("picture")
     .sf(rrect(0.5, 1.5, 15.5, 14.5, 2.0), B)
     .sf(Path().m(1.0, 12.5).l(5.5, 7.5).l(9.0, 11.0).l(11.0, 9.0).l(15.0, 13.0).l(15.0, 13.5).l(1.0, 13.5).z(), A, A)
     .f(circle(11.0, 5.5, 1.75), A))

    # Link: two rings of a chain
    ring1 = [(x + 5.5, y + 10.5) for x, y in rot([(-5.0, -2.5), (2.0, -2.5), (2.0, 2.5), (-5.0, 2.5)], 0, 0, -45)]
    ic = icon("link")
    ic.s(Path().m(7.0, 5.0).l(9.25, 2.75).arc(11.375, 4.875, 3.0, 225, 405).l(11.0, 9.0), A)
    ic.s(Path().m(9.0, 11.0).l(6.75, 13.25).arc(4.625, 11.125, 3.0, 45, 225).l(5.0, 7.0), A)
    ic.s(line((5.75, 10.25), (10.25, 5.75)), B)

    # Date & Time: a calendar with a clock
    ic = icon("datetime")
    ic.sf(rrect(0.5, 2.0, 12.5, 13.5, 1.5), B)
    ic.s(line((0.5, 5.5), (12.5, 5.5)), B).s(line((3.5, 0.5), (3.5, 3.0)), B).s(line((9.5, 0.5), (9.5, 3.0)), B)
    ic.x(circle(11.5, 11.5, 4.75))
    ic.sf(circle(11.5, 11.5, 4.0), A)
    ic.s(line((11.5, 9.25), (11.5, 11.75), (13.25, 11.75)), A)

    # Symbol: an omega
    (icon("symbol")
     .s(Path().m(1.5, 14.5).l(5.5, 14.5).l(5.5, 12.5).arc(8.0, 7.5, 5.5, 117, 423).l(10.5, 14.5).l(14.5, 14.5), B))

    # Margins: a page and the dashed edges inside
    ic = icon("margins")
    ic.sf(rrect(2.0, 0.5, 14.0, 15.5, 1.5), B)
    for x in (4.75, 11.25):
        for y0 in (2.5, 5.5, 8.5, 11.5):
            ic.s(line((x, y0), (x, y0 + 1.5)), A)
    # Orientation: a page upright and one lying
    (icon("orientation")
     .sf(rrect(0.5, 0.5, 8.5, 12.5, 1.25), B)
     .x(rrect(4.5, 6.5, 16.5, 16.5, 2.0))
     .sf(rrect(5.5, 7.5, 15.5, 15.5, 1.25), A))
    # Size: a page and its measure
    ic = icon("size")
    page(ic, 4.0, 0.5, 15.0, 15.5)
    ic.s(line((1.5, 1.0), (1.5, 15.0)), A).s(line((0.0, 1.0), (3.0, 1.0)), A).s(line((0.0, 15.0), (3.0, 15.0)), A)

    # the views: Print Layout, Web Layout, Read Mode
    ic = icon("printlayout")
    ic.sf(rrect(2.5, 0.5, 13.5, 15.5, 1.5), B)
    lines(ic, [(5.0, 4.5, 11.0), (5.0, 7.5, 11.0), (5.0, 10.5, 9.0)])
    (icon("weblayout")
     .sf(circle(8.0, 8.0, 7.0), B)
     .s(Path().arc(8.0, 8.0, 3.0, 0, 360, 7.0), B)
     .s(line((1.0, 8.0), (15.0, 8.0)), B)
     .s(Path().m(2.0, 4.5).l(14.0, 4.5), B).s(Path().m(2.0, 11.5).l(14.0, 11.5), B))
    (icon("readmode")
     .sf(Path().m(8.0, 3.0).c(6.0, 1.75, 3.5, 1.5, 0.5, 2.0).l(0.5, 13.0).c(3.5, 12.5, 6.0, 12.75, 8.0, 14.0).z(), B)
     .sf(Path().m(8.0, 3.0).c(10.0, 1.75, 12.5, 1.5, 15.5, 2.0).l(15.5, 13.0).c(12.5, 12.5, 10.0, 12.75, 8.0, 14.0).z(), A))

    # Ruler
    ic = icon("ruler")
    ic.sf(rrect(0.5, 4.5, 15.5, 11.5, 1.25), B)
    for x, h in ((3.5, 3.0), (6.0, 2.0), (8.5, 3.0), (11.0, 2.0), (13.0, 3.0)):
        ic.s(line((x, 4.5), (x, 4.5 + h)), A)

    # Zoom, One Page, Multiple Pages, Page Width
    (icon("zoom")
     .sf(circle(6.5, 6.5, 5.5), B)
     .s(line((10.5, 10.5), (15.25, 15.25)), B, 1.0, "round")
     .s(line((6.5, 4.0), (6.5, 9.0)), A).s(line((4.0, 6.5), (9.0, 6.5)), A))
    ic = icon("onepage")
    ic.sf(rrect(3.5, 0.5, 12.5, 15.5, 1.5), B)
    lines(ic, [(5.75, 4.5, 10.25), (5.75, 7.5, 10.25)], A)
    ic = icon("multipage")
    ic.sf(rrect(0.5, 2.5, 7.0, 13.5, 1.25), B).sf(rrect(9.0, 2.5, 15.5, 13.5, 1.25), B)
    lines(ic, [(2.5, 5.5, 5.0), (11.0, 5.5, 13.5)], A)
    ic = icon("pagewidth")
    ic.sf(rrect(3.5, 0.5, 12.5, 15.5, 1.5), B)
    ic.s(line((0.5, 8.0), (15.5, 8.0)), A).s(line((2.5, 6.0), (0.5, 8.0), (2.5, 10.0)), A).s(
        line((13.5, 6.0), (15.5, 8.0), (13.5, 10.0)), A)

    # Save: the disk; Save As: the disk and a pen
    def disk(ic):
        ic.sf(Path().m(0.5, 2.5).arc(2.5, 2.5, 2.0, 180, 270).l(12.0, 0.5).l(15.5, 4.0).l(15.5, 13.5)
              .arc(13.5, 13.5, 2.0, 0, 90).l(2.5, 15.5).arc(2.5, 13.5, 2.0, 90, 180).z(), B)
        ic.s(Path().m(4.5, 0.5).l(4.5, 4.0).arc(5.5, 4.0, 1.0, 180, 90).l(10.5, 5.0).arc(10.5, 4.0, 1.0, 90, 0).l(11.5, 0.5), B)
        ic.sf(Path().m(3.5, 15.5).l(3.5, 10.0).arc(4.5, 10.0, 1.0, 180, 270).l(11.5, 9.0).arc(11.5, 10.0, 1.0, 270, 360)
              .l(12.5, 15.5).z(), A)
        return ic
    disk(icon("save"))
    ic = disk(icon("saveas"))
    pen = [(x + 12.0, y + 11.0) for x, y in rot([(-1.6, -5.5), (1.6, -5.5), (1.6, 3.0), (0.0, 5.5), (-1.6, 3.0)], 0, 0, 45)]
    ic.x(poly(*pen), w=2.0)
    ic.sf(poly(*pen), B)

    # the File page: Home, New, Info, Close, Back, Options, Export
    (icon("home")
     .sf(Path().m(8.0, 0.75).l(15.25, 7.0).l(13.5, 7.0).l(13.5, 15.0).l(9.75, 15.0).l(9.75, 10.5).l(6.25, 10.5)
         .l(6.25, 15.0).l(2.5, 15.0).l(2.5, 7.0).l(0.75, 7.0).z(), B))
    ic = icon("newdoc")
    page(ic, 2.0, 0.5, 14.0, 15.5)
    ic.x(circle(12.0, 12.0, 4.5))
    mi.badge(ic, 12.0, 12.0, 3.75, A, 0.0)
    ic.s(line((12.0, 10.0), (12.0, 14.0)), K, 1.2).s(line((10.0, 12.0), (14.0, 12.0)), K, 1.2)
    (icon("info")
     .s(circle(8.0, 8.0, 7.0), B)
     .s(line((8.0, 7.0), (8.0, 11.75)), A)
     .f(circle(8.0, 4.75, 0.9), A))
    icon("close").s(line((2.5, 2.5), (13.5, 13.5)), B).s(line((13.5, 2.5), (2.5, 13.5)), B)
    (icon("back")
     .s(circle(8.0, 8.0, 7.0), B)
     .s(line((11.5, 8.0), (4.5, 8.0)), B).s(line((7.5, 5.0), (4.5, 8.0), (7.5, 11.0)), B))
    ic = icon("options")
    mi.gear(ic, 8.0, 8.0, 7.5, B, 8, 0.0)
    ic = icon("export")
    page(ic, 1.0, 0.5, 11.5, 15.5)
    ic.x(line((8.0, 10.0), (16.0, 10.0)), w=3.0)
    ic.s(line((7.5, 10.0), (15.0, 10.0)), A).s(line((12.0, 7.0), (15.0, 10.0), (12.0, 13.0)), A)

    # the status bar: Focus
    (icon("focus")
     .s(line((0.5, 5.0), (0.5, 0.5), (5.0, 0.5)), B).s(line((11.0, 0.5), (15.5, 0.5), (15.5, 5.0)), B)
     .s(line((0.5, 11.0), (0.5, 15.5), (5.0, 15.5)), B).s(line((11.0, 15.5), (15.5, 15.5), (15.5, 11.0)), B)
     .f(circle(8.0, 8.0, 2.5), A))

    # the document's own picture, as a list shows it
    ic = icon("document")
    page(ic, 2.0, 0.5, 14.0, 15.5)
    lines(ic, [(4.5, 7.0, 11.5), (4.5, 9.5, 11.5), (4.5, 12.0, 9.0)], A)


def sheet(path):
    from PIL import Image, ImageDraw
    cell = 170
    cols = 6
    rows = (len(ICONS) + cols - 1) // cols
    im = Image.new("RGB", (cols * cell, rows * 100), (255, 255, 255))
    d = ImageDraw.Draw(im)
    for n, ic in enumerate(ICONS):
        x0, y0 = (n % cols) * cell, (n // cols) * 100
        d.text((x0 + 4, y0 + 86), ic.name, fill=(0, 0, 0))
        im.paste(mi.compose(ic.render(32), mi.LIGHT, (255, 255, 255)).resize((64, 64), Image.NEAREST), (x0 + 4, y0 + 4))
        for j, s in enumerate((16, 24)):
            im.paste(mi.compose(ic.render(s), mi.LIGHT, (255, 255, 255)), (x0 + 74 + j * 20, y0 + 4))
            im.paste(mi.compose(ic.render(s), mi.DARK, (41, 41, 41)), (x0 + 74 + j * 20, y0 + 40))
        im.paste(mi.compose(ic.render(48), mi.LIGHT, (255, 255, 255)), (x0 + 116, y0 + 30))
    im.save(path)


def build():
    blob = bytearray(b"AMI1")
    blob += struct.pack("<HB", len(ICONS), len(SIZES)) + bytes(SIZES)
    for ic in ICONS:
        canvases = [ic.render(s) for s in SIZES]
        mask = 0
        for cv in canvases:
            for r in range(6):
                if (cv.owner == r).any():
                    mask |= 1 << r
        name = ic.name.encode("ascii")
        blob += bytes((len(name),)) + name + bytes((mask,))
        for cv in canvases:
            for r in range(6):
                if mask & (1 << r):
                    blob += mi.rle(cv.coverage(r).tobytes())
    return bytes(blob)


def main():
    define()
    if "--only" in sys.argv:
        names = sys.argv[sys.argv.index("--only") + 1].split(",")
        ICONS[:] = [ic for ic in ICONS if ic.name in names]
    if "--sheet" in sys.argv:
        sheet(sys.argv[sys.argv.index("--sheet") + 1])
        return
    blob = build()
    with open(OUT, "wb") as f:
        f.write(blob)
    print(f"{OUT}: {len(ICONS)} pictures, {len(blob)} bytes")


if __name__ == "__main__":
    main()
