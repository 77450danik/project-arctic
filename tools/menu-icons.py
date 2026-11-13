#!/usr/bin/env python3
"""The pictures of the shell's menu items, in Windows 11's way.

Windows 11's context menus give each of the system's commands a small line
picture: grey lines, blue where the command acts (the blades of the scissors
grey, their rings blue), white inside closed shapes. Those pictures are not
ours to ship, so they are drawn here: each is a 16-unit design of lines one
unit wide, as Segoe Fluent Icons are, sampled 16 x 16 per pixel at every size
a menu uses (100 % to 200 %).

A picture keeps its parts apart by role, not by colour: the lines (role B),
the blue (A), the white inside (W), fixed white (K), fixed dark (D) and fixed
mid grey (E). shell32 paints them in the colours of the menu it goes in, so
one set serves light and dark menus alike.

Output: runtime/reactos/files/dll/win32/shell32/res/menuicons.bin, read by
shell32's menuicons.cpp. Format (little endian):
    "AMI1", u16 icons, u8 sizes, u8 size[sizes]
    per icon: u8 name length, name (ASCII), u8 roles (bit mask),
              per size, per role in the mask: run-length coverage,
              pairs of (u8 zeros, u8 count, count bytes) until size*size.

    tools/menu-icons.py                  writes the resource
    tools/menu-icons.py --sheet out.png  draws every picture for review
                       [--ref DIR]       next to DIR/<codepoint>.png (32 px)
"""
import math
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "runtime/reactos/files/dll/win32/shell32/res/menuicons.bin")
SIZES = (16, 20, 24, 28, 32)
SS = 16                       # samples per pixel and axis
B, A, W, K, D, E = range(6)   # roles
CLEAR = -1

# colours of the roles for the review sheet: light menu, dark menu
LIGHT = {B: (0x42, 0x42, 0x42), A: (0x00, 0x78, 0xD4), W: (0xFF, 0xFF, 0xFF), K: (0xFF, 0xFF, 0xFF),
         D: (0x2B, 0x2B, 0x2B), E: (0x6E, 0x6E, 0x6E)}
DARK = {B: (0xDE, 0xDE, 0xDE), A: (0x4C, 0xC2, 0xFF), W: None, K: (0xFF, 0xFF, 0xFF),
        D: (0x2B, 0x2B, 0x2B), E: (0x6E, 0x6E, 0x6E)}


# ---------------------------------------------------------------- paths

class Path:
    """A path in design units; arcs and curves are flattened as they come."""

    def __init__(self):
        self.subpaths = []
        self.closed = []

    def m(self, x, y):
        self.subpaths.append([(x, y)])
        self.closed.append(False)
        return self

    def l(self, x, y):
        self.subpaths[-1].append((x, y))
        return self

    def arc(self, cx, cy, r, a0, a1, ry=None):
        """from angle a0 to a1 in degrees, 0 = right, 90 = down"""
        ry = r if ry is None else ry
        n = max(8, int(abs(a1 - a0) / 3))
        pts = [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
                cy + ry * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]
        if not self.subpaths:
            self.m(*pts[0])
        self.subpaths[-1].extend(pts)
        return self

    def c(self, x1, y1, x2, y2, x, y):
        x0, y0 = self.subpaths[-1][-1]
        for i in range(1, 33):
            t = i / 32
            u = 1 - t
            self.subpaths[-1].append((u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x,
                                      u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y))
        return self

    def q(self, x1, y1, x, y):
        x0, y0 = self.subpaths[-1][-1]
        return self.c(x0 + 2 / 3 * (x1 - x0), y0 + 2 / 3 * (y1 - y0), x + 2 / 3 * (x1 - x), y + 2 / 3 * (y1 - y), x, y)

    def z(self):
        self.closed[-1] = True
        return self


def line(*pts):
    p = Path().m(*pts[0])
    for pt in pts[1:]:
        p.l(*pt)
    return p


def poly(*pts):
    return line(*pts).z()


def rrect(x0, y0, x1, y1, r):
    if r <= 0:
        return poly((x0, y0), (x1, y0), (x1, y1), (x0, y1))
    return (Path().m(x0 + r, y0).l(x1 - r, y0).arc(x1 - r, y0 + r, r, 270, 360)
            .l(x1, y1 - r).arc(x1 - r, y1 - r, r, 0, 90).l(x0 + r, y1)
            .arc(x0 + r, y1 - r, r, 90, 180).l(x0, y0 + r).arc(x0 + r, y0 + r, r, 180, 270).z())


def circle(cx, cy, r):
    return Path().arc(cx, cy, r, 0, 360).z()


def rot(pts, cx, cy, deg):
    a = math.radians(deg)
    return [(cx + (x - cx) * math.cos(a) - (y - cy) * math.sin(a),
             cy + (x - cx) * math.sin(a) + (y - cy) * math.cos(a)) for x, y in pts]


# ---------------------------------------------------------------- drawing

class Canvas:
    """Who owns each sample: a role, or -1 for nothing."""

    def __init__(self, size):
        self.size = size
        self.n = size * SS
        self.unit = self.n / 16.0
        self.owner = np.full((self.n, self.n), -1, np.int8)

    def _mask(self, draw_fn):
        im = Image.new("L", (self.n, self.n), 0)
        draw_fn(ImageDraw.Draw(im))
        return np.asarray(im) > 127

    def _pts(self, pts):
        return [(x * self.unit, y * self.unit) for x, y in pts]

    def stroke_mask(self, path, w=1.0, cap="flat"):
        width = max(1, int(round(w * self.unit)))
        rad = w * self.unit / 2

        def draw(d):
            for pts, closed in zip(path.subpaths, path.closed):
                p = self._pts(pts + ([pts[0], pts[1]] if closed and len(pts) > 1 else []))
                if len(p) == 1:
                    p = p * 2
                d.line(p, fill=255, width=width, joint="curve")
                if cap == "round" and not closed:
                    for x, y in (p[0], p[-1]):
                        d.ellipse((x - rad, y - rad, x + rad, y + rad), fill=255)
                # joints of a polyline: round them so that bends stay full
                for x, y in p[1:-1]:
                    d.ellipse((x - rad, y - rad, x + rad, y + rad), fill=255)
        return self._mask(draw)

    def fill_mask(self, path):
        def draw(d):
            for pts in path.subpaths:
                if len(pts) > 2:
                    d.polygon(self._pts(pts), fill=255)
        return self._mask(draw)

    def paint(self, mask, role):
        self.owner[mask] = role

    def coverage(self, role):
        m = (self.owner == role).reshape(self.size, SS, self.size, SS)
        return (m.mean(axis=(1, 3)) * 255 + 0.5).astype(np.uint8)


def erode(mask, radius):
    """shrink a mask by about radius samples (an octagon, near enough a disc)"""
    m = mask.copy()
    for i in range(int(round(radius))):
        n = m.copy()
        n[1:, :] &= m[:-1, :]
        n[:-1, :] &= m[1:, :]
        n[:, 1:] &= m[:, :-1]
        n[:, :-1] &= m[:, 1:]
        if i % 2:
            n[1:, 1:] &= m[:-1, :-1]
            n[:-1, :-1] &= m[1:, 1:]
            n[1:, :-1] &= m[:-1, 1:]
            n[:-1, 1:] &= m[1:, :-1]
        m = n
    return m


def dilate(mask, radius):
    return ~erode(~mask, radius)


class Icon:
    """A picture: a list of operations, run on a canvas of each size."""

    def __init__(self, name, ref=None):
        self.name = name
        self.ref = ref
        self.ops = []

    # lines of one role
    def s(self, path, role=B, w=1.0, cap="flat"):
        self.ops.append(("stroke", path, role, w, cap))
        return self

    # the inside of a closed path
    def f(self, path, role=W):
        self.ops.append(("fill", path, role))
        return self

    # a closed shape: its inside white (or another role), its edge in lines
    def sf(self, path, role=B, fill=W, w=1.0):
        self.ops.append(("fill", path, fill))
        self.ops.append(("stroke", path, role, w, "flat"))
        return self

    # take away what is under a path's stroke (w) or inside it (w=None)
    def x(self, path, w=None, cap="round"):
        if w is None:
            self.ops.append(("fill", path, CLEAR))
        else:
            self.ops.append(("stroke", path, CLEAR, w, cap))
        return self

    # a silhouette made of shapes (+) and holes (-), drawn as an outline one
    # unit inside its edge, the inside filled; gap clears around it first
    def outline(self, parts, role=B, fill=W, gap=0.0, w=1.0):
        self.ops.append(("outline", parts, role, fill, gap, w))
        return self

    def render(self, size):
        cv = Canvas(size)
        for op in self.ops:
            kind = op[0]
            if kind == "stroke":
                _, path, role, w, cap = op
                cv.paint(cv.stroke_mask(path, w, cap), role)
            elif kind == "fill":
                _, path, role = op
                cv.paint(cv.fill_mask(path), role)
            elif kind == "outline":
                _, parts, role, fill, gap, w = op
                sil = np.zeros((cv.n, cv.n), bool)
                for sign, path in parts:
                    m = cv.fill_mask(path) if isinstance(path, Path) else path(cv)
                    sil = (sil | m) if sign == "+" else (sil & ~m)
                if gap:
                    cv.paint(dilate(sil, gap * cv.unit), CLEAR)
                inner = erode(sil, w * cv.unit)
                if fill is not None:
                    cv.paint(inner, fill)
                cv.paint(sil & ~inner, role)
        return cv


ICONS = []


def icon(name, ref=None):
    i = Icon(name, ref)
    ICONS.append(i)
    return i


def stroke_shape(path, w=1.0, cap="flat"):
    """a stroke as a silhouette part"""
    return lambda cv: cv.stroke_mask(path, w, cap)


# ---------------------------------------------------------------- the pictures

def folder_path(x0=0.5, y0=1.5, x1=15.5, y1=14.5, tab=6.0):
    """Segoe Fluent's folder: a tab at the top left over the body"""
    r = 1.5
    return (Path().m(x0, y0 + r).arc(x0 + r, y0 + r, r, 180, 270).l(x0 + tab - 1.0, y0)
            .arc(x0 + tab - 1.0, y0 + 1.0, 1.0, 270, 315).l(x0 + tab + 1.3, y0 + 2.5)
            .l(x1 - r, y0 + 2.5).arc(x1 - r, y0 + 2.5 + r, r, 270, 360).l(x1, y1 - r)
            .arc(x1 - r, y1 - r, r, 0, 90).l(x0 + r, y1).arc(x0 + r, y1 - r, r, 90, 180).z())


def folder(ic, role=B, fill=W, **kw):
    p = folder_path(**kw)
    ic.sf(p, role, fill)
    x0 = kw.get("x0", 0.5)
    y0 = kw.get("y0", 1.5)
    tab = kw.get("tab", 6.0)
    # the line under the tab
    ic.s(line((x0, y0 + 4.5 - 1.0), (x0 + tab - 0.5, y0 + 4.5 - 1.0), (x0 + tab + 1.3, y0 + 2.5)), role)
    return ic


def window(ic, x0, y0, x1, y1, role=B, fill=W, bar=True):
    ic.sf(rrect(x0, y0, x1, y1, 1.5), role, fill)
    if bar:
        ic.s(line((x0, y0 + 2.5), (x1, y0 + 2.5)), role)
    return ic


def shield_path(cx, top, w, h):
    """a shield: flat top with shoulders, sides down into a point"""
    hw = w / 2
    return (Path().m(cx, top).c(cx - hw * 0.45, top + 1.2, cx - hw * 0.8, top + 1.4, cx - hw, top + 1.4)
            .l(cx - hw, top + h * 0.5).c(cx - hw, top + h * 0.8, cx - hw * 0.4, top + h * 0.95, cx, top + h)
            .c(cx + hw * 0.4, top + h * 0.95, cx + hw, top + h * 0.8, cx + hw, top + h * 0.5)
            .l(cx + hw, top + 1.4).c(cx + hw * 0.8, top + 1.4, cx + hw * 0.45, top + 1.2, cx, top).z())


def gear(ic, cx, cy, r, role=A, teeth=8, gap=1.0):
    """a filled gear with a hole, the hole cut through to what is below"""
    def body(cv):
        m = cv.fill_mask(circle(cx, cy, r * 0.72))
        for i in range(teeth):
            a = 360 / teeth * i
            t = rot([(cx - r * 0.2, cy - r), (cx + r * 0.2, cy - r), (cx + r * 0.2, cy), (cx - r * 0.2, cy)], cx, cy, a)
            m |= cv.fill_mask(poly(*t))
        return m
    ic.outline([("+", body)], role, fill=role, gap=gap)
    ic.x(circle(cx, cy, r * 0.3))
    return ic


def badge(ic, cx, cy, r, role=A, gap=1.0):
    ic.x(circle(cx, cy, r + gap))
    ic.f(circle(cx, cy, r), role)
    return ic


def define():
    # Cut: the blades grey, the rings blue
    (icon("cut", "e8c6")
     .s(line((3.5, 0.5), (10.15, 11.2)), B)
     .s(line((12.5, 0.5), (5.85, 11.2)), B)
     .sf(circle(4.0, 13.0, 2.5), A)
     .sf(circle(12.0, 13.0, 2.5), A))

    # Copy: the copy in front blue, the one behind grey
    (icon("copy", "e8c8")
     .s(Path().m(5.0, 4.5).l(3.5, 4.5).arc(3.5, 6.5, 2.0, 270, 180).l(1.5, 12.5)
        .arc(3.5, 12.5, 2.0, 180, 90).l(7.0, 14.5).arc(7.0, 12.5, 2.0, 90, 15), B)
     .sf(rrect(6.5, 1.5, 14.5, 11.5, 2.0), A))

    # Paste: the clipboard grey, the sheet on it blue
    (icon("paste", "e77f")
     .f(rrect(1.5, 1.5, 12.5, 15.5, 1.5), W)
     .s(Path().m(4.5, 1.5).l(3.0, 1.5).arc(3.0, 3.0, 1.5, 270, 180).l(1.5, 14.0)
        .arc(3.0, 14.0, 1.5, 180, 90).l(5.5, 15.5), B)
     .s(Path().m(9.5, 1.5).l(11.0, 1.5).arc(11.0, 3.0, 1.5, 270, 360).l(12.5, 3.5), B)
     .sf(rrect(4.5, 0.5, 9.5, 2.5, 1.0), B)
     .x(rrect(7.5, 5.5, 14.5, 15.5, 1.5), w=3)
     .x(rrect(7.5, 5.5, 14.5, 15.5, 1.5))
     .sf(rrect(7.5, 5.5, 14.5, 15.5, 1.5), A))

    # Paste shortcut: the clipboard with the shortcut's arrow
    (icon("pastelink", "e77f")
     .f(rrect(1.5, 1.5, 12.5, 15.5, 1.5), W)
     .s(Path().m(4.5, 1.5).l(3.0, 1.5).arc(3.0, 3.0, 1.5, 270, 180).l(1.5, 14.0)
        .arc(3.0, 14.0, 1.5, 180, 90).l(5.5, 15.5), B)
     .s(Path().m(9.5, 1.5).l(11.0, 1.5).arc(11.0, 3.0, 1.5, 270, 360).l(12.5, 3.5), B)
     .sf(rrect(4.5, 0.5, 9.5, 2.5, 1.0), B)
     .x(rrect(7.5, 5.5, 14.5, 15.5, 1.5), w=3)
     .x(rrect(7.5, 5.5, 14.5, 15.5, 1.5))
     .sf(rrect(7.5, 5.5, 14.5, 15.5, 1.5), B)
     .s(line((9.75, 13.25), (12.5, 10.5)), A)
     .s(line((10.0, 8.75), (12.75, 8.75), (12.75, 11.5)), A))

    # Rename: a field, the letter and the caret blue
    (icon("rename", "e8ac")
     .sf(rrect(0.5, 2.5, 15.5, 13.5, 2.0), B)
     .x(line((10.5, 1.0), (10.5, 15.0)), w=3, cap="flat")
     .s(line((10.5, 0.5), (10.5, 15.5)), A)
     .s(line((8.5, 0.5), (12.5, 0.5)), A)
     .s(line((8.5, 15.5), (12.5, 15.5)), A)
     .s(line((2.6, 10.6), (5.25, 4.25), (7.9, 10.6)), A)
     .s(line((3.6, 8.6), (6.9, 8.6)), A))

    # Share: an open box, the arrow leaving it blue
    (icon("share", "e72d")
     .s(Path().m(6.5, 2.5).l(3.5, 2.5).arc(3.5, 4.5, 2.0, 270, 180).l(1.5, 12.5)
        .arc(3.5, 12.5, 2.0, 180, 90).l(11.5, 14.5).arc(11.5, 12.5, 2.0, 90, 0).l(13.5, 11.0), B)
     .sf(Path().m(4.1, 10.4).c(4.2, 6.2, 6.8, 3.9, 10.5, 3.9).l(10.5, 0.9).l(15.2, 5.8).l(10.5, 10.6)
         .l(10.5, 7.9).c(7.8, 7.9, 5.6, 8.6, 4.1, 10.4).z(), A))

    # Delete: the bin, all grey
    (icon("delete", "e74d")
     .f(poly((2.0, 2.5), (14.0, 2.5), (13.0, 14.0), (11.5, 15.5), (4.5, 15.5), (3.0, 14.0)), W)
     .s(Path().m(2.0, 3.0).l(3.0, 14.0).arc(4.5, 14.0, 1.5, 180, 90).l(11.5, 15.5)
        .arc(11.5, 14.0, 1.5, 90, 0).l(14.0, 3.0), B)
     .s(line((0.75, 2.5), (15.25, 2.5)), B)
     .s(Path().m(6.0, 2.5).l(6.0, 1.75).arc(7.25, 1.75, 1.25, 180, 270).l(8.75, 0.5)
        .arc(8.75, 1.75, 1.25, 270, 360).l(10.0, 2.5), B)
     .s(line((6.5, 6.0), (6.5, 11.75)), B)
     .s(line((9.5, 6.0), (9.5, 11.75)), B))

    # Properties: the wrench of Windows 11's menus
    (icon("properties", "e90f")
     .sf(Path().m(0.95, 12.75).l(6.5, 7.1).l(6.5, 4.25).c(6.5, 2.0, 8.2, 0.5, 10.6, 0.5).l(12.4, 0.75)
         .l(10.2, 4.1).c(9.9, 4.7, 10.6, 5.6, 11.4, 5.6).c(12.2, 5.6, 12.7, 5.0, 13.3, 4.4).l(15.5, 2.5)
         .l(15.5, 5.4).c(15.5, 7.6, 13.3, 9.25, 10.8, 9.25).l(9.6, 9.25).l(3.6, 15.1)
         .c(2.6, 16.0, 0.6, 15.6, 0.6, 14.0).z(), B))

    # Open with: a list of programs and a blue badge
    ic = icon("openwith", "e7ac")
    for y in (1.5, 6.5, 11.5):
        ic.sf(rrect(1.5, y, 4.5, y + 3.0, 0.75), B)
    ic.s(line((6.5, 2.5), (14.75, 2.5)), B).s(line((6.5, 7.5), (14.75, 7.5)), B)
    badge(ic, 11.75, 11.75, 4.25)
    ic.s(line((11.75, 14.0), (11.75, 9.75)), K, 1.25).s(line((9.75, 11.6), (11.75, 9.6), (13.75, 11.6)), K, 1.25)

    # Copy as path: a field with a path in it
    (icon("copypath")
     .sf(rrect(0.5, 2.5, 15.5, 13.5, 2.0), B)
     .s(line((3.6, 5.5), (5.4, 10.5)), A)
     .s(line((6.1, 5.5), (7.9, 10.5)), A)
     .f(circle(10.4, 10.25, 0.75), A)
     .f(circle(12.9, 10.25, 0.75), A))

    # Compress to ZIP: a folder and its zipper
    ic = icon("zip", "f012")
    folder(ic)
    ic.x(line((11.5, 3.5), (11.5, 14.5)), w=2.6, cap="flat")
    ic.s(line((10.5, 4.0), (10.5, 15.0)), A).s(line((12.5, 4.0), (12.5, 15.0)), A)
    for y in (5.5, 7.5, 9.5, 11.5, 13.5):
        ic.s(line((10.5, y), (12.5, y)), A, 0.9)

    # Pin: Windows 11's pins are blue
    pin = poly((8.4, 1.1), (9.2, 0.65), (10.0, 1.1), (15.35, 6.5), (15.25, 7.15), (9.55, 9.65), (9.15, 12.85),
               (8.5, 13.75), (0.9, 7.0), (1.2, 6.8), (6.45, 5.0))
    (icon("pin", "e718").sf(pin, A)
     .s(line((4.85, 10.55), (0.05, 15.35)), A))
    (icon("unpin", "e718").sf(pin, A)
     .s(line((4.85, 10.55), (0.05, 15.35)), A)
     .x(line((1.0, 1.0), (15.0, 15.0)), w=3.0)
     .s(line((1.0, 1.0), (15.0, 15.0)), B))

    # Add to favourites
    icon("favorite", "e734").sf(poly((8.0, 0.9), (10.4, 5.6), (15.3, 6.3), (11.95, 9.85), (12.5, 15.0),
                                     (8.0, 12.7), (3.5, 15.0), (4.05, 9.85), (0.7, 6.3), (5.6, 5.6)), A)

    # Run as administrator: a window and the blue shield
    ic = icon("runas", "e7ef")
    window(ic, 1.5, 1.5, 14.5, 14.0)
    ic.outline([("+", shield_path(12.0, 7.0, 7.0, 8.75))], A, fill=A, gap=1.0)

    # the shield alone (Manage)
    icon("shield", "ea18").outline([("+", shield_path(8.0, 0.25, 13.0, 15.5))], B, fill=W).outline(
        [("+", shield_path(8.0, 3.0, 7.6, 10.0))], A, fill=A)

    # Open file location, Open in a folder
    folder(icon("folder", "e8b7"))

    # View: four tiles
    ic = icon("view", "e8a9")
    for x, y in ((1.5, 1.5), (9.5, 1.5), (1.5, 9.5), (9.5, 9.5)):
        ic.s(rrect(x, y, x + 5.0, y + 5.0, 1.25), B)

    # Sort by: up grey, down blue
    (icon("sort", "e8cb")
     .s(line((4.5, 2.6), (4.5, 13.5)), B).s(line((0.9, 6.3), (4.5, 2.6), (8.1, 6.3)), B)
     .s(line((11.5, 2.5), (11.5, 13.4)), A).s(line((7.9, 9.7), (11.5, 13.4), (15.1, 9.7)), A))

    # Refresh
    (icon("refresh", "e72c")
     .s(Path().arc(8.0, 8.0, 7.5, 4, 310), B)
     .s(line((14.5, 0.75), (14.5, 4.5), (10.75, 4.5)), B))

    # Undo, Redo, Restore
    undo = (Path().m(3.9, 3.6).arc(9.15, 6.4, 5.6, 208, 368).l(4.3, 15.6))
    icon("undo", "e7a7").s(undo, A).s(line((2.5, 0.75), (2.5, 5.5), (7.25, 5.5)), A)
    redo = Path()
    redo.subpaths = [[(16 - x, y) for x, y in undo.subpaths[0]]]
    redo.closed = [False]
    icon("redo", "e7a6").s(redo, A).s(line((13.5, 0.75), (13.5, 5.5), (8.75, 5.5)), A)

    # New: a ring and the blue plus
    (icon("new", "ecc8")
     .s(circle(8.0, 8.0, 7.0), B)
     .s(line((8.0, 4.25), (8.0, 11.75)), A).s(line((4.25, 8.0), (11.75, 8.0)), A))

    # Display settings: the screen and a blue gear
    ic = icon("display", "e7f4")
    ic.sf(rrect(0.5, 1.5, 15.5, 12.0, 1.5), B)
    ic.s(line((4.5, 12.0), (4.5, 14.5)), B).s(line((2.0, 14.5), (7.0, 14.5)), B)
    gear(ic, 12.0, 12.0, 3.6)

    # Personalize: the pen across the whole square, its point blue
    body = [(x + 8.0, y + 8.0) for x, y in rot([(-1.75, -10.0), (1.75, -10.0), (1.75, 5.2), (-1.75, 5.2)], 0, 0, 45)]
    tip = [(x + 8.0, y + 8.0) for x, y in rot([(-1.75, 5.2), (1.75, 5.2), (0.0, 9.6)], 0, 0, 45)]
    (icon("personalize", "e771")
     .sf(poly(*body), B)
     .sf(poly(*tip), A, A))

    # Open in Terminal: the terminal's own picture
    (icon("terminal", "e756")
     .f(rrect(0.5, 1.5, 15.5, 14.5, 2.0), D)
     .s(rrect(0.5, 1.5, 15.5, 14.5, 2.0), E, 1.0)
     .s(line((4.0, 5.5), (7.0, 8.0), (4.0, 10.5)), K, 1.2)
     .s(line((8.5, 10.75), (12.0, 10.75)), K, 1.2))

    # the desktop's View submenu: large, medium and small icons
    (icon("viewlarge")
     .s(rrect(1.5, 1.5, 14.5, 11.5, 1.5), B)
     .s(line((1.5, 14.5), (14.5, 14.5)), B))
    (icon("viewmedium")
     .s(rrect(3.5, 1.5, 12.5, 9.5, 1.5), B)
     .s(line((3.5, 12.0), (12.5, 12.0)), B)
     .s(line((5.5, 14.5), (10.5, 14.5)), B))
    ic = icon("viewsmall", "e8a9")
    for x, y in ((2.5, 2.5), (9.5, 2.5), (2.5, 9.5), (9.5, 9.5)):
        ic.s(rrect(x, y, x + 4.0, y + 4.0, 1.25), B)
    # a folder's: list, details, tiles
    ic = icon("viewlist", "e8fd")
    for y in (3.0, 8.0, 13.0):
        ic.f(circle(2.25, y, 1.0), B).s(line((5.5, y), (15.0, y)), B)
    ic = icon("viewdetails")
    for y in (2.5, 8.0, 13.5):
        ic.s(rrect(1.0, y - 1.5, 4.0, y + 1.5, 0.75), B).s(line((6.0, y), (10.5, y)), B).s(line((12.5, y), (15.0, y)), B)
    ic = icon("viewtiles", "eca5")
    for y in (1.5, 9.5):
        ic.s(rrect(1.5, y, 6.5, y + 5.0, 1.25), B).s(line((8.5, y + 1.25), (14.75, y + 1.25)), B).s(
            line((8.5, y + 3.75), (12.75, y + 3.75)), B)
    # Auto arrange icons, Align icons to grid
    sparkle = [(8.0 + (5.0 if i % 2 == 0 else 1.3) * math.sin(math.radians(45 * i)),
                8.0 - (5.0 if i % 2 == 0 else 1.3) * math.cos(math.radians(45 * i))) for i in range(8)]
    (icon("autoarrange", "f1ba")
     .sf(rrect(1.5, 1.5, 14.5, 14.5, 2.0), B)
     .f(poly(*sparkle), A))
    (icon("aligngrid")
     .s(line((0.5, 3.5), (15.5, 3.5)), B, 1.0)
     .s(line((3.5, 0.5), (3.5, 15.5)), B, 1.0)
     .sf(rrect(5.5, 5.5, 14.5, 14.5, 1.5), B))

    # Send to
    (icon("sendto")
     .s(Path().m(1.5, 13.5).c(1.5, 9.0, 4.0, 6.5, 8.5, 6.5).l(14.5, 6.5), B)
     .s(line((10.5, 2.5), (14.5, 6.5), (10.5, 10.5)), B))

    # Create shortcut
    (icon("shortcut")
     .sf(rrect(1.5, 1.5, 14.5, 14.5, 2.0), B)
     .s(line((5.5, 10.5), (10.5, 5.5)), A)
     .s(line((6.5, 5.5), (10.5, 5.5), (10.5, 9.5)), A))

    # Move to, Copy to: a folder with a blue badge
    ic = icon("moveto", "e8de")
    folder(ic)
    badge(ic, 11.75, 11.75, 4.25)
    ic.s(line((9.6, 11.75), (13.6, 11.75)), K, 1.25).s(line((11.75, 9.75), (13.75, 11.75), (11.75, 13.75)), K, 1.25)
    ic = icon("copyto", "e8de")
    folder(ic)
    badge(ic, 11.75, 11.75, 4.25)
    ic.s(line((11.75, 9.5), (11.75, 14.0)), K, 1.25).s(line((9.5, 11.75), (14.0, 11.75)), K, 1.25)

    # Search
    (icon("search", "e721")
     .s(circle(6.5, 6.5, 5.5), B)
     .s(line((10.5, 10.5), (15.25, 15.25)), B, 1.0, "round"))

    # Open in the tree (Explore): a window with its folder pane blue
    ic = icon("explore", "e8a0")
    ic.sf(rrect(0.5, 1.5, 15.5, 14.5, 1.5), B)
    ic.s(line((5.5, 1.5), (5.5, 14.5)), B)
    for y in (5.0, 8.0, 11.0):
        ic.s(line((2.25, y), (3.75, y)), A)

    # Give access to: two people, the one in front blue
    (icon("people", "e716")
     .s(circle(14.0 - 3.0, 4.0, 2.25), B)
     .s(Path().m(13.0, 8.0).l(13.5, 8.0).arc(13.5, 10.0, 2.0, 270, 360).l(15.5, 11.0)
        .c(15.5, 12.5, 14.0, 13.0, 12.0, 13.0), B)
     .x(circle(6.0, 3.5, 3.6))
     .s(circle(6.0, 3.5, 2.75), A)
     .x(rrect(-0.5, 7.0, 12.5, 16.0, 3.0))
     .sf(Path().m(1.5, 8.5).l(10.5, 8.5).arc(10.5, 10.0, 1.5, 270, 360).l(12.0, 11.0)
         .c(12.0, 13.5, 9.5, 14.5, 6.0, 14.5).c(2.5, 14.5, 0.5, 13.5, 0.5, 11.0).l(0.5, 9.5)
         .arc(1.5, 9.5, 1.0, 180, 270).z(), A))

    # Format: a drive being made anew
    (icon("format", "ebc4")
     .sf(rrect(0.5, 9.0, 15.5, 15.0, 1.5), B)
     .s(line((11.5, 12.0), (13.0, 12.0)), B)
     .s(Path().arc(8.0, 4.75, 3.25, 200, 470), A)
     .s(line((3.6, 2.4), (4.75, 4.6), (7.0, 3.6)), A))

    # Map network drive, Disconnect network drive
    def netdrive(ic):
        ic.sf(rrect(1.5, 1.5, 14.5, 6.5, 1.5), B)
        ic.s(line((8.0, 6.5), (8.0, 10.5)), B)
        ic.sf(rrect(6.0, 10.5, 10.0, 14.0, 0.75), A)
        ic.s(line((0.5, 12.25), (6.0, 12.25)), A).s(line((10.0, 12.25), (15.5, 12.25)), A)
        return ic
    netdrive(icon("mapdrive", "e8ce"))
    (netdrive(icon("unmapdrive", "e8cd"))
     .x(line((1.0, 1.0), (15.0, 15.0)), w=3.0)
     .s(line((1.0, 1.0), (15.0, 15.0)), B))

    # Print
    (icon("print", "e749")
     .sf(rrect(3.5, 1.5, 12.5, 6.0, 1.0), B)
     .sf(Path().m(3.5, 11.5).l(1.5, 11.5).arc(1.5, 10.0, 1.5, 90, 180).l(0.0, 5.5)
         .arc(1.5, 5.5, 1.5, 180, 270).l(14.5, 4.0).arc(14.5, 5.5, 1.5, 270, 360).l(16.0, 10.0)
         .arc(14.5, 10.0, 1.5, 0, 90).l(12.5, 11.5), B)
     .x(rrect(3.5, 8.5, 12.5, 15.0, 1.0), w=1.0)
     .sf(rrect(3.5, 8.5, 12.5, 15.0, 1.0), A))

    # Edit (with no program of its own)
    def pencil(cv):
        pts = rot([(-1.75, -8.0), (1.75, -8.0), (1.75, 4.5), (0.0, 7.6), (-1.75, 4.5)], 0, 0, 45)
        return cv.fill_mask(poly(*[(x + 8.3, y + 7.7) for x, y in pts]))
    icon("edit", "e70f").outline([("+", pencil)], B).s(line(*rot([(6.55, 2.3), (10.05, 2.3)], 8.3, 7.7, 45)), A)

    # Open (with no program of its own): a box with an arrow out of it
    (icon("open", "e8a7")
     .s(Path().m(7.5, 1.5).l(3.5, 1.5).arc(3.5, 3.5, 2.0, 270, 180).l(1.5, 12.5)
        .arc(3.5, 12.5, 2.0, 180, 90).l(12.5, 14.5).arc(12.5, 12.5, 2.0, 90, 0).l(14.5, 8.5), B)
     .s(line((7.5, 8.5), (14.0, 2.0)), A)
     .s(line((9.75, 1.5), (14.5, 1.5), (14.5, 6.25)), A))

    # Eject
    (icon("eject")
     .sf(poly((8.0, 2.0), (14.5, 9.0), (1.5, 9.0)), B)
     .s(line((1.5, 13.5), (14.5, 13.5)), A))

    # Remove access (sharing): a lock
    (icon("lock")
     .s(Path().m(4.5, 7.0).l(4.5, 4.5).arc(8.0, 4.5, 3.5, 180, 360).l(11.5, 7.0), B)
     .sf(rrect(2.5, 7.0, 13.5, 15.0, 1.5), B)
     .f(circle(8.0, 11.0, 1.25), A))


# ---------------------------------------------------------------- output

def rle(data):
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        z = 0
        while i < n and data[i] == 0 and z < 255:
            z += 1
            i += 1
        j = i
        while j < n and j - i < 255 and not (data[j] == 0 and (j + 1 >= n or data[j + 1] == 0)):
            j += 1
        out += bytes((z, j - i)) + bytes(data[i:j])
        i = j
    return bytes(out)


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
                    blob += rle(cv.coverage(r).tobytes())
    return bytes(blob)


def compose(cv, colours, bg):
    """the picture over a background, as a menu shows it"""
    out = np.array(bg, float).reshape(1, 1, 3).repeat(cv.size, 0).repeat(cv.size, 1)
    for r in range(6):
        if colours[r] is None:
            continue
        a = cv.coverage(r).astype(float)[..., None] / 255
        out = out * (1 - a) + np.array(colours[r], float) * a
    return Image.fromarray(out.clip(0, 255).astype(np.uint8), "RGB")


def sheet(path, refdir):
    cell = 230
    cols = 4
    rows = (len(ICONS) + cols - 1) // cols
    im = Image.new("RGB", (cols * cell, rows * 120), (255, 255, 255))
    d = ImageDraw.Draw(im)
    for n, ic in enumerate(ICONS):
        x0, y0 = (n % cols) * cell, (n // cols) * 120
        d.text((x0 + 4, y0 + 104), ic.name + (" " + ic.ref if ic.ref else ""), fill=(0, 0, 0))
        cv32 = ic.render(32)
        im.paste(compose(cv32, LIGHT, (243, 243, 243)).resize((64, 64), Image.NEAREST), (x0 + 4, y0 + 4))
        if refdir and ic.ref and os.path.exists(os.path.join(refdir, ic.ref + ".png")):
            ref = Image.open(os.path.join(refdir, ic.ref + ".png")).convert("RGB")
            im.paste(ref.resize((64, 64), Image.NEAREST), (x0 + 72, y0 + 4))
            # overlay: the reference red, ours green
            rv = 1 - np.asarray(ref, float).mean(axis=2) / 255
            ov = sum(cv32.coverage(r).astype(float) / 255 for r in range(6) if r != W)
            o = np.stack([255 - ov * 255, 255 - rv * 255, 255 - (rv + ov).clip(0, 1) * 255], -1)
            im.paste(Image.fromarray(o.clip(0, 255).astype(np.uint8)).resize((64, 64), Image.NEAREST), (x0 + 140, y0 + 4))
        for k, (colours, bg) in enumerate(((LIGHT, (243, 243, 243)), (DARK, (44, 44, 44)))):
            for j, s in enumerate((16, 20, 24)):
                pic = compose(ic.render(s), colours, bg)
                im.paste(pic, (x0 + 4 + j * 28 + k * 90, y0 + 74))
    im.save(path)


def main():
    define()
    if "--only" in sys.argv:
        names = sys.argv[sys.argv.index("--only") + 1].split(",")
        ICONS[:] = [ic for ic in ICONS if ic.name in names]
    if "--sheet" in sys.argv:
        refdir = sys.argv[sys.argv.index("--ref") + 1] if "--ref" in sys.argv else None
        sheet(sys.argv[sys.argv.index("--sheet") + 1], refdir)
        return
    blob = build()
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(blob)
    print(f"{OUT}: {len(ICONS)} pictures, {len(blob)} bytes")


if __name__ == "__main__":
    main()
