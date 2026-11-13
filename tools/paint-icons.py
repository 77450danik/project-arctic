#!/usr/bin/env python3
"""The pictures of Paint's and Photos' buttons, menus and status bars.

They are drawn as the pictures of the shell's menus are (tools/menu-icons.py,
whose designs and roles this takes): a 16-unit design of lines one unit
wide, in the colour of the text, blue where the command acts, as Windows 11's
Paint and Photos have theirs (Segoe Fluent Icons, which are not ours to ship).
Those the menus have already (Cut, Copy, Paste, Undo, Redo, Print, Delete...)
are taken from there.

Output: icons.bin, the "AMI1" format of menu-icons.py, into
runtime/wine/modules/programs/mspaint and .../photos (one set for both).

    tools/paint-icons.py                  writes the resources
    tools/paint-icons.py --sheet out.png  draws every picture for review
"""
import importlib.util
import math
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTS = [os.path.join(ROOT, "runtime/wine/modules/programs", p, "icons.bin") for p in ("mspaint", "photos")]

_spec = importlib.util.spec_from_file_location("menu_icons", os.path.join(ROOT, "tools/menu-icons.py"))
mi = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mi)

Path, line, poly, rrect, circle, rot = mi.Path, mi.line, mi.poly, mi.rrect, mi.circle, mi.rot
B, A, W, K, D, E = mi.B, mi.A, mi.W, mi.K, mi.D, mi.E
SIZES = (16, 20, 24, 28, 30, 32, 36, 40, 48)

ICONS = []
TAKEN = ("cut", "copy", "paste", "undo", "redo", "print", "delete", "properties", "new", "open", "openwith",
         "copypath", "edit", "folder", "favorite", "share")


def icon(name):
    i = mi.Icon(name)
    ICONS.append(i)
    return i


# ---------------------------------------------------------------- helpers

def dashes(ic, pts, closed=True, dash=2.0, gap=1.5, role=B):
    """a dashed polyline"""
    seq = list(pts) + ([pts[0]] if closed else [])
    on, left = True, dash
    cur = [seq[0]]
    for (x0, y0), (x1, y1) in zip(seq, seq[1:]):
        seg = math.hypot(x1 - x0, y1 - y0)
        t = 0.0
        while seg - t > 1e-6:
            step = min(left, seg - t)
            t += step
            p = (x0 + (x1 - x0) * t / seg, y0 + (y1 - y0) * t / seg)
            left -= step
            if on:
                cur.append(p)
            if left <= 1e-6:
                if on and len(cur) > 1:
                    ic.s(line(*cur), role)
                on = not on
                left = dash if on else gap
                cur = [p]
    if on and len(cur) > 1:
        ic.s(line(*cur), role)
    return ic


def dashed_rect(ic, x0, y0, x1, y1, **kw):
    return dashes(ic, [(x0, y0), (x1, y0), (x1, y1), (x0, y1)], **kw)


def turned(pts, deg, cx=8.0, cy=8.0):
    return rot(pts, cx, cy, deg)


def tool_body(ic, outline_pts, deg, role=B, fill=W):
    ic.sf(poly(*turned(outline_pts, deg)), role, fill)


def pencil(ic, deg=-45, role=B):
    """a pencil along x, its tip at the left, then turned"""
    tool_body(ic, [(5, 6), (15, 6), (15, 10), (5, 10)], deg, role)
    ic.sf(poly(*turned([(1, 8), (5, 6), (5, 10)], deg)), role, W)
    ic.f(poly(*turned([(1, 8), (2.8, 7.1), (2.8, 8.9)], deg)), role)
    ic.s(line(*turned([(12.5, 6), (12.5, 10)], deg)), role)
    return ic


def brush(ic, head_role=W, deg=-45):
    """a round brush: bristles, the metal, the handle"""
    ic.sf(Path().m(*turned([(1.0, 8)], deg)[0]).l(*turned([(3.5, 6.0)], deg)[0]).l(*turned([(6.5, 6.0)], deg)[0])
          .l(*turned([(6.5, 10.0)], deg)[0]).l(*turned([(3.5, 10.0)], deg)[0]).z(), B, head_role)
    ic.f(poly(*turned([(6.5, 6.25), (9, 6.25), (9, 9.75), (6.5, 9.75)], deg)), B)
    ic.sf(poly(*turned([(9, 7), (15.5, 7), (15.5, 9), (9, 9)], deg)), B, W)
    return ic


def magnifier(ic, inner=None):
    ic.sf(circle(6.5, 6.5, 5.0), B, W)
    ic.s(line((10.2, 10.2), (15, 15)), B, 1.6, "round")
    if inner == "+":
        ic.s(line((4, 6.5), (9, 6.5)), B)
        ic.s(line((6.5, 4), (6.5, 9)), B)
    elif inner == "-":
        ic.s(line((4, 6.5), (9, 6.5)), B)
    return ic


def ellipse_pts(cx, cy, rx, ry, a0, a1, n):
    return [(cx + rx * math.cos(a0 + (a1 - a0) * i / n), cy + ry * math.sin(a0 + (a1 - a0) * i / n)) for i in range(n + 1)]


def star_pts(cx, cy, rx, ry, points, inner):
    return [(cx + rx * (inner if i % 2 else 1) * math.cos(-math.pi / 2 + math.pi * i / points),
             cy + ry * (inner if i % 2 else 1) * math.sin(-math.pi / 2 + math.pi * i / points)) for i in range(points * 2)]


def heart_path(cx, top, w, h):
    pts = []
    for i in range(80):
        s = 2 * math.pi * i / 80
        hx = 16 * math.sin(s) ** 3
        hy = 13 * math.cos(s) - 5 * math.cos(2 * s) - 2 * math.cos(3 * s) - math.cos(4 * s)
        pts.append((cx + hx / 16 * w / 2, top + (12 - hy) / 29 * h))
    return poly(*pts)


def floppy(ic, role=B):
    ic.sf(Path().m(1.5, 3).arc(3, 3, 1.5, 180, 270).l(12, 1.5).l(14.5, 4).l(14.5, 13)
          .arc(13, 13, 1.5, 0, 90).l(3, 14.5).arc(3, 13, 1.5, 90, 180).z(), role, W)
    ic.s(line((4.5, 1.5), (4.5, 5.5), (10.5, 5.5), (10.5, 1.5)), role)
    ic.s(line((4.5, 14.5), (4.5, 9.5), (11.5, 9.5), (11.5, 14.5)), role)
    return ic


# ---------------------------------------------------------------- the pictures

def define():
    mi.define()
    for name in TAKEN:
        ICONS.append(next(ic for ic in mi.ICONS if ic.name == name))

    # Selection
    dashed_rect(icon("select_rect"), 1.5, 2.5, 14.5, 13.5)
    blob = ellipse_pts(8, 8, 6.5, 5.5, 0, 2 * math.pi, 48)
    blob = [(x + 0.8 * math.sin(3 * a), y) for (x, y), a in zip(blob, [2 * math.pi * i / 48 for i in range(49)])]
    dashes(icon("select_free"), blob[:-1])
    ic = icon("selectall")
    dashed_rect(ic, 1.5, 1.5, 14.5, 14.5)
    ic.f(rrect(4.5, 4.5, 11.5, 11.5, 1), A)
    ic = icon("invertsel")
    ic.sf(rrect(1.5, 1.5, 14.5, 14.5, 1.5), B, W)
    ic.f(rrect(1.5, 1.5, 14.5, 14.5, 1.5), A)
    ic.x(rrect(4.5, 4.5, 11.5, 11.5, 0))
    dashed_rect(ic, 4.5, 4.5, 11.5, 11.5)

    # Image
    ic = icon("crop")
    ic.s(line((4, 0.5), (4, 12), (15.5, 12)), B)
    ic.s(line((0.5, 4), (12, 4), (12, 15.5)), B)
    ic = icon("resize")
    ic.sf(rrect(1.5, 7.5, 8.5, 14.5, 1), B, W)
    ic.s(line((6.5, 9.5), (14, 2)), A)
    ic.s(line((9, 2), (14, 2), (14, 7)), A)

    def rot_arrow(ic, cw=True):
        if cw:
            ic.s(Path().arc(8, 8, 5.5, 200, 470), A)
            ic.s(line((7.5, 13.5), (8, 13.5)), A)
            end = (8 + 5.5 * math.cos(math.radians(470)), 8 + 5.5 * math.sin(math.radians(470)))
            ic.s(line((end[0] + 3, end[1] - 2.5), end, (end[0] + 3, end[1] + 2.5)), A)
        return ic

    ic = icon("rotate")
    ic.s(Path().arc(8, 8, 6, -150, 150), B)
    ic.s(line((1.8, 1.8), (2.8, 5.0), (6.0, 4.0)), B)
    ic = icon("rotright")
    ic.sf(rrect(1.5, 6.5, 9.5, 14.5, 1), B, W)
    ic.s(Path().arc(9.5, 6.5, 5, 180, 300), A)
    ic.s(line((10.5, 0.7), (12.0, 2.2), (10.6, 3.9)), A)
    ic = icon("rotleft")
    ic.sf(rrect(6.5, 6.5, 14.5, 14.5, 1), B, W)
    ic.s(Path().arc(6.5, 6.5, 5, 240, 360), A)
    ic.s(line((5.5, 0.7), (4.0, 2.2), (5.4, 3.9)), A)
    ic = icon("flipv")
    ic.sf(poly((2, 6.5), (14, 6.5), (8, 1.5)), B, W)
    ic.sf(poly((2, 9.5), (14, 9.5), (8, 14.5)), B, A)
    ic = icon("fliph")
    ic.sf(poly((6.5, 2), (6.5, 14), (1.5, 8)), B, W)
    ic.sf(poly((9.5, 2), (9.5, 14), (14.5, 8)), B, A)

    # Tools
    pencil(icon("pencil"))
    ic = icon("bucket")
    tool_body(ic, [(2.5, 5.5), (10.5, 5.5), (10.5, 13.5), (2.5, 13.5)], 45, B, W)
    ic.s(line(*turned([(2.5, 9.5), (10.5, 9.5)], 45)), B)
    ic.f(Path().m(14, 8.5).l(15.6, 12).arc(14, 12.6, 1.6, 0, 180).z(), A)
    ic = icon("text")
    ic.s(line((2.5, 14.5), (8, 1.5), (13.5, 14.5)), B)
    ic.s(line((4.7, 10), (11.3, 10)), B)
    ic = icon("eraser")
    tool_body(ic, [(1.5, 5.5), (14.5, 5.5), (14.5, 10.5), (1.5, 10.5)], -45, B, W)
    ic.f(poly(*turned([(1.5, 5.5), (6.5, 5.5), (6.5, 10.5), (1.5, 10.5)], -45)), A)
    ic.s(line((7, 15), (15, 15)), B)
    ic = icon("picker")
    ic.s(line((1.8, 14.2), (3, 13), (10, 6)), B, 1.6, "round")
    ic.sf(circle(12, 4, 2.6), B, B)
    ic.s(line((8, 4.5), (11.5, 8)), B, 1.2)
    magnifier(icon("magnifier"))

    # Brushes
    brush(icon("brush"))
    ic = brush(icon("callig1"))
    ic.s(line((10, 15), (15, 10)), A, 1.5)
    ic = icon("callig2")
    ic.sf(poly((8, 1), (13, 7), (8, 15), (3, 7)), B, W)
    ic.s(line((8, 9), (8, 15)), B)
    ic.f(circle(8, 8, 1.2), B)
    ic = icon("airbrush")
    ic.sf(rrect(2.5, 6.5, 9.5, 15, 1.5), B, W)
    ic.sf(rrect(4.5, 3, 7.5, 6.5, 0.5), B, W)
    for x, y in ((11.5, 2.5), (13.5, 4.5), (11.5, 6.5), (14.5, 7.5), (12.5, 9.5)):
        ic.f(circle(x, y, 0.75), A)
    brush(icon("oilbrush"), head_role=A)
    ic = icon("crayon")
    tool_body(ic, [(5, 5.5), (15, 5.5), (15, 10.5), (5, 10.5)], -45, B, W)
    ic.sf(poly(*turned([(1, 8), (5, 5.5), (5, 10.5)], -45)), B, A)
    ic.s(line(*turned([(9, 5.5), (9, 10.5)], -45)), B)
    ic.s(line(*turned([(11, 5.5), (11, 10.5)], -45)), B)
    ic = icon("marker")
    tool_body(ic, [(4.5, 5), (15, 5), (15, 11), (4.5, 11)], -45, B, W)
    ic.sf(poly(*turned([(1, 7), (4.5, 6), (4.5, 10), (1, 9)], -45)), B, A)
    ic.s(line(*turned([(11, 5), (11, 11)], -45)), B)
    ic = pencil(icon("natpencil"))
    ic.s(line((9.5, 14.5), (11, 13)), A)
    ic.s(line((11.5, 14.5), (13, 13)), A)
    ic.s(line((13.5, 14.5), (15, 13)), A)
    ic = brush(icon("watercolor"))
    ic.f(Path().m(13, 8.5).l(14.8, 12).arc(13, 12.7, 1.8, 0, 180).z(), A)

    # Shapes' outline and fill, the size
    ic = icon("outline")
    ic.s(rrect(1.5, 2.5, 12.5, 13.5, 1.5), B, 1.6)
    ic = icon("fillshape")
    ic.sf(rrect(1.5, 2.5, 12.5, 13.5, 1.5), B, A)
    ic = icon("size")
    for y, w in ((2.5, 0.7), (6, 1.1), (10, 1.8), (14, 2.6)):
        ic.s(line((1.5, y), (14.5, y)), B, w, "round")

    # the gallery of shapes, in a box of 1.5 .. 14.5
    l, t, r, b = 1.5, 2.5, 14.5, 13.5
    w, h, cx, cy = r - l, b - t, (l + r) / 2, (t + b) / 2
    icon("sh_line").s(line((2, 14), (14, 2)), B)
    icon("sh_curve").s(Path().m(1.5, 13).c(5, -2, 11, 18, 14.5, 3), B)
    icon("sh_oval").s(Path().arc(8, 8, 6.5, 0, 360, 5.0).z(), B)
    icon("sh_rect").s(rrect(l, t, r, b, 0), B)
    icon("sh_rrect").s(rrect(l, t, r, b, 2.5), B)
    icon("sh_polygon").s(poly((2, 13.5), (2.5, 5), (8.5, 2), (14, 7), (11, 14)), B)
    icon("sh_triangle").s(poly((8, 2), (14.5, 14), (1.5, 14)), B)
    icon("sh_rtriangle").s(poly((2, 2), (14, 14), (2, 14)), B)
    icon("sh_diamond").s(poly((8, 1.5), (14.5, 8), (8, 14.5), (1.5, 8)), B)
    pent = [(8 + 6.8 * math.cos(-math.pi / 2 + 2 * math.pi * i / 5), 8.6 + 6.8 * math.sin(-math.pi / 2 + 2 * math.pi * i / 5)) for i in range(5)]
    icon("sh_pentagon").s(poly(*pent), B)
    icon("sh_hexagon").s(poly((l + w / 4, t), (r - w / 4, t), (r, cy), (r - w / 4, b), (l + w / 4, b), (l, cy)), B)
    unit = [(0, 0.25), (0.55, 0.25), (0.55, 0), (1, 0.5), (0.55, 1), (0.55, 0.75), (0, 0.75)]
    icon("sh_aright").s(poly(*[(l + u * w, t + v * h) for u, v in unit]), B)
    icon("sh_aleft").s(poly(*[(r - u * w, t + v * h) for u, v in unit]), B)
    icon("sh_aup").s(poly(*[(1.5 + v * 13, 14.5 - u * 13) for u, v in unit]), B)
    icon("sh_adown").s(poly(*[(1.5 + v * 13, 1.5 + u * 13) for u, v in unit]), B)
    icon("sh_star4").s(poly(*star_pts(8, 8, 6.8, 6.8, 4, 0.38)), B)
    icon("sh_star5").s(poly(*star_pts(8, 8.6, 7.0, 7.0, 5, 0.42)), B)
    icon("sh_star6").s(poly(*star_pts(8, 8, 6.8, 6.8, 6, 0.55)), B)
    icon("sh_crect").s(Path().m(3, 2.5).l(13, 2.5).arc(13, 4, 1.5, 270, 360).l(14.5, 9.5).arc(13, 9.5, 1.5, 0, 90)
                       .l(7, 11).l(3.5, 14.5).l(4.5, 11).l(3, 11).arc(3, 9.5, 1.5, 90, 180).l(1.5, 4)
                       .arc(3, 4, 1.5, 180, 270).z(), B)
    co = ellipse_pts(8, 7, 6.5, 4.8, 0.62 * math.pi, 2.38 * math.pi, 40) + [(3, 14.5)]
    icon("sh_coval").s(poly(*co), B)
    ic = icon("sh_ccloud")
    for i in range(7):
        a = 2 * math.pi * i / 7
        ic.s(Path().arc(8 + 4.6 * math.cos(a), 7 + 3.3 * math.sin(a), 2.4, math.degrees(a) - 80, math.degrees(a) + 80), B)
    ic.s(circle(3.8, 13.2, 1.0), B)
    ic.s(circle(1.9, 15.0, 0.5), B, 0.8)
    icon("sh_heart").s(heart_path(8, 2.2, 13.5, 12.3), B)
    lz = [(0.38, 0), (0.62, 0.30), (0.53, 0.34), (0.80, 0.60), (0.72, 0.64), (1.0, 1.0), (0.48, 0.70), (0.58, 0.66),
          (0.22, 0.44), (0.32, 0.40), (0.0, 0.15)]
    icon("sh_lightning").s(poly(*[(1.5 + u * 13, 1.5 + v * 13) for u, v in lz]), B)

    # the status bar
    icon("st_cursor").sf(poly((4, 1.5), (4, 13), (6.8, 10.3), (9, 14.8), (10.8, 14), (8.7, 9.6), (12.5, 9.5)), B, W)
    dashed_rect(icon("st_select"), 1.5, 3.5, 14.5, 12.5)
    ic = icon("st_canvas")
    ic.sf(rrect(1.5, 1.5, 12, 12, 1), B, W)
    ic.f(rrect(12.5, 12.5, 15.5, 15.5, 0.5), A)
    floppy(icon("st_disk"))

    # files and the window
    floppy(icon("save"))
    ic = floppy(icon("saveas"))
    ic.x(circle(13, 13, 3.6))
    ic.f(circle(13, 13, 2.8), A)
    ic = icon("import")
    ic.s(line((1.5, 10), (1.5, 14.5), (14.5, 14.5), (14.5, 10)), B)
    ic.s(line((8, 1.5), (8, 11)), A)
    ic.s(line((4.5, 7.5), (8, 11), (11.5, 7.5)), A)
    ic = icon("recent")
    ic.sf(circle(8, 8, 6.5), B, W)
    ic.s(line((8, 4), (8, 8), (11, 10)), A)
    ic = icon("wallpaper")
    ic.sf(rrect(1.5, 2, 14.5, 11.5, 1), B, W)
    ic.f(poly((3.5, 10), (6.5, 6), (8.5, 8.5), (10, 7), (12.5, 10)), A)
    ic.s(line((8, 11.5), (8, 14.5)), B)
    ic.s(line((5, 14.5), (11, 14.5)), B)
    ic = icon("about")
    ic.sf(circle(8, 8, 6.8), B, W)
    ic.s(line((8, 7), (8, 11.8)), B)
    ic.f(circle(8, 4.6, 0.8), B)
    icon("info").ops = list(ic.ops)
    ic = icon("settings")
    mi.gear(ic, 8, 8, 7.4, role=B, teeth=8, gap=0)
    magnifier(icon("zoomin"), "+")
    magnifier(icon("zoomout"), "-")
    ic = magnifier(icon("zoom100"))
    ic.f(circle(6.5, 6.5, 1.5), A)
    ic = icon("grid")
    ic.s(rrect(1.5, 1.5, 14.5, 14.5, 1.5), B)
    for v in (5.8, 10.2):
        ic.s(line((v, 1.5), (v, 14.5)), B)
        ic.s(line((1.5, v), (14.5, v)), B)
    ic = icon("fit")
    for sx, sy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
        x0, y0 = 8 - sx * 6.5, 8 - sy * 6.5
        ic.s(line((x0, y0 + sy * 4), (x0, y0), (x0 + sx * 4, y0)), B)
    ic = icon("fullscreen")
    for sx, sy in ((1, 1), (-1, 1), (1, -1), (-1, -1)):
        x0, y0 = 8 - sx * 6.5, 8 - sy * 6.5
        ic.s(line((x0, y0 + sy * 4), (x0, y0), (x0 + sx * 4, y0)), B)
        ic.s(line((x0 + sx * 0.5, y0 + sy * 0.5), (x0 + sx * 4, y0 + sy * 4)), B)
    ic = icon("bg_clear")
    ic.s(rrect(1.5, 1.5, 14.5, 14.5, 1.5), B)
    for x, y in ((2, 2), (8, 8), (8, 2), (2, 8)):
        if (x + y) % 12 == 4 or (x, y) in ((2, 2), (8, 8)):
            ic.f(rrect(x, y, x + 6, y + 6, 0), B)
    ic = icon("bg_opaque")
    ic.sf(rrect(1.5, 1.5, 14.5, 14.5, 1.5), B, A)

    # Photos
    ic = icon("filmstrip")
    ic.sf(rrect(0.5, 3.5, 15.5, 12.5, 1.5), B, W)
    ic.s(line((5.5, 3.5), (5.5, 12.5)), B)
    ic.s(line((10.5, 3.5), (10.5, 12.5)), B)
    ic = icon("slideshow")
    ic.sf(rrect(1.5, 2.5, 14.5, 13.5, 1.5), B, W)
    ic.f(poly((6.5, 5), (11, 8), (6.5, 11)), A)
    icon("heart").sf(heart_path(8, 2.2, 14, 12.6), B, W)
    icon("heart_on").sf(heart_path(8, 2.2, 14, 12.6), A, A)
    ic = icon("photo")
    ic.sf(rrect(1.5, 2.5, 14.5, 13.5, 1.5), B, W)
    ic.s(line((1.5, 11), (5.5, 7), (9, 10.5), (11, 8.5), (14.5, 12)), B)
    ic.f(circle(10.5, 5.5, 1.2), A)
    ic = icon("editimage")
    ic.sf(rrect(0.5, 2.5, 11.5, 12.5, 1.5), B, W)
    ic.s(line((0.5, 10.5), (4, 7), (7, 10)), B)
    ic.x(poly(*turned([(5, 4.5), (17, 4.5), (17, 11.5), (5, 11.5)], -45, 11.5, 11.5)))
    pencil_pts = turned([(6, 6.5), (16, 6.5), (16, 9.5), (6, 9.5)], -45, 11.5, 11.5)
    ic.sf(poly(*pencil_pts), A, W)


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
    for out in OUTS:
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, "wb") as f:
            f.write(blob)
        print(f"{out}: {len(ICONS)} pictures, {len(blob)} bytes")


if __name__ == "__main__":
    main()
