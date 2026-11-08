#!/usr/bin/env python3
"""Mizu's "Windows 11" size: rounded controls, drawn here.

The visual style of Arctic is ReactOS's Mizu. Its fourth size, "Windows 11"
(ReactOS patch "mizu-the-windows-11-style"), keeps Mizu's title bars, taskbar
and everything else, and gives the controls of programs the shapes and
colours of Windows 11: push buttons, check boxes, radio buttons, combo boxes,
edit fields, scroll bars, sliders, progress bars, tabs, group boxes and
toolbar buttons. Nothing here comes from Windows: every picture is drawn by
this script, four times over and scaled down for its soft corners, with an
alpha channel (uxtheme premultiplies it and blends).

Its interface font is Inter (runtime/fonts).

usage: make-win11-mizu.py MIZU_SOURCE_DIR OUT_DIR
  MIZU_SOURCE_DIR  ReactOS's media/themes/Mizu/mizu.msstyles, with the dark
                   scheme's NormalDark.INI in its textfiles
  OUT_DIR          runtime/reactos/files/media/themes/Mizu/mizu.msstyles
"""
import os
import re
import sys

from PIL import Image, ImageDraw

SS = 4  # drawn this many times larger, then scaled down

LIGHT = {
    "prefix": "W11L",
    "base": "NormalNormal.INI",
    "target": "Windows11Normal.INI",
    "text": (27, 27, 27),
    "text_pressed": (93, 93, 93),
    "text_disabled": (160, 160, 160),
    # the fills of a button, normal hot pressed disabled
    "fill": ((253, 253, 253), (246, 246, 246), (243, 243, 243), (245, 245, 245)),
    "sides": (229, 229, 229),
    "top": None,
    "bottom": (204, 204, 204),
    "field": (255, 255, 255),         # COLOR_WINDOW: what the text of a field is drawn on
    "field_hot": (250, 250, 250),
    "field_disabled": (245, 245, 245),
    "underline": (141, 141, 141),
    "accent": ((0, 95, 184), (26, 111, 192), (51, 127, 198), (191, 191, 191)),
    "on_accent": (255, 255, 255),
    "box_fill": ((247, 247, 247), (240, 240, 240), (229, 229, 229), (249, 249, 249)),
    "box_line": ((138, 138, 138), (122, 122, 122), (158, 158, 158), (191, 191, 191)),
    "glyph": ((93, 93, 93), (60, 60, 60), (93, 93, 93), (160, 160, 160)),
    "thumb": ((138, 138, 138), (118, 118, 118), (98, 98, 98), (200, 200, 200)),
    "track": (250, 250, 250),
    "face": (240, 240, 240),          # COLOR_BTNFACE: what stands behind a tab
    "slider": (176, 176, 176),
    "knob": (255, 255, 255),
    "knob_line": (213, 213, 213),
    "progress_track": (230, 230, 230),
    "surface": (255, 255, 255),       # of a tab's page
    "tint": (0, 0, 0),                # what a toolbar button is shaded with
    "tint_alpha": (0, 14, 23, 0, 23, 31),
}

DARK = {
    "prefix": "W11D",
    "base": "NormalDark.INI",
    "target": "Windows11Dark.INI",
    "text": (255, 255, 255),
    "text_pressed": (200, 200, 200),
    "text_disabled": (120, 120, 120),
    "fill": ((45, 45, 45), (50, 50, 50), (39, 39, 39), (42, 42, 42)),
    "sides": (51, 51, 51),
    "top": (60, 60, 60),
    "bottom": None,
    "field": (25, 25, 25),
    "field_hot": (32, 32, 32),
    "field_disabled": (37, 37, 37),
    "underline": (154, 154, 154),
    "accent": ((96, 205, 255), (91, 189, 235), (86, 173, 217), (90, 90, 90)),
    "on_accent": (0, 0, 0),
    "box_fill": ((28, 28, 28), (42, 42, 42), (51, 51, 51), (37, 37, 37)),
    "box_line": ((154, 154, 154), (170, 170, 170), (130, 130, 130), (93, 93, 93)),
    "glyph": ((200, 200, 200), (255, 255, 255), (200, 200, 200), (120, 120, 120)),
    "thumb": ((158, 158, 158), (176, 176, 176), (192, 192, 192), (90, 90, 90)),
    "track": (25, 25, 25),
    "face": (32, 32, 32),
    "slider": (110, 110, 110),
    "knob": (69, 69, 69),
    "knob_line": (82, 82, 82),
    "progress_track": (64, 64, 64),
    "surface": (43, 43, 43),
    "tint": (255, 255, 255),
    "tint_alpha": (0, 20, 13, 0, 23, 31),
}


def canvas(w, h, rgb=(0, 0, 0)):
    return Image.new("RGBA", (w * SS, h * SS), tuple(rgb) + (0,))


def box(x0, y0, x1, y1):
    """A rectangle of whole pixels, for ImageDraw, whose corners are inclusive."""
    return (x0 * SS, y0 * SS, x1 * SS - 1, y1 * SS - 1)


def plate(w, h, r, fill, sides, top=None, bottom=None, line=1.0, x=0, y=0, cw=None, ch=None):
    """A rounded plate: fill inside a line of colour sides, its top or bottom edge another colour."""
    cw = cw if cw is not None else w
    ch = ch if ch is not None else h
    im = canvas(cw, ch, sides)
    d = ImageDraw.Draw(im)
    x1, y1 = x + w, y + h
    rad = r * SS
    if bottom:
        d.rounded_rectangle(box(x, y, x1, y1), rad, fill=tuple(bottom) + (255,))
        d.rounded_rectangle(box(x, y, x1, y1 - 1), rad, fill=tuple(sides) + (255,))
    elif top:
        d.rounded_rectangle(box(x, y, x1, y1), rad, fill=tuple(top) + (255,))
        d.rounded_rectangle(box(x, y + 1, x1, y1), rad, fill=tuple(sides) + (255,))
    else:
        d.rounded_rectangle(box(x, y, x1, y1), rad, fill=tuple(sides) + (255,))
    inset = int(round(line * SS))
    d.rounded_rectangle((x * SS + inset, y * SS + inset, x1 * SS - 1 - inset, y1 * SS - 1 - inset),
                        max(0, rad - inset), fill=tuple(fill) + (255,))
    return im


def shrink(im):
    return im.resize((im.size[0] // SS, im.size[1] // SS), Image.BOX)


def stack(frames):
    frames = [shrink(f) for f in frames]
    w, h = frames[0].size
    out = Image.new("RGBA", (w, h * len(frames)), (0, 0, 0, 0))
    for i, f in enumerate(frames):
        out.paste(f, (0, i * h))
    return out


def save(im, out, name):
    path = os.path.join(out, "bitmaps", name + ".bmp")
    if im.getchannel("A").getextrema() == (255, 255):
        im.convert("RGB").save(path, "BMP")
    else:
        # 32-bit: BI_RGB with its alpha, which Pillow writes as is
        im.save(path, "BMP")
    return name


def button(c):
    frames = []
    for i in range(4):
        pressed = i == 2
        frames.append(plate(24, 24, 4, c["fill"][i], c["sides"], c["top"] if not pressed else None,
                            c["bottom"] if i < 2 else None))
    # the default button: the normal one in the accent's line
    frames.append(plate(24, 24, 4, c["fill"][0], c["accent"][0], line=1.25))
    return stack(frames)


def tick(d, s, colour):
    w = max(SS, int(round(s * SS * 0.125)))
    points = [(s * SS * 0.25, s * SS * 0.53), (s * SS * 0.43, s * SS * 0.71), (s * SS * 0.76, s * SS * 0.31)]
    d.line(points, fill=tuple(colour) + (255,), width=w, joint="curve")
    for p in (points[0], points[-1]):
        d.ellipse((p[0] - w / 2, p[1] - w / 2, p[0] + w / 2, p[1] + w / 2), fill=tuple(colour) + (255,))


def dash(d, s, colour):
    w = max(SS, int(round(s * SS * 0.125)))
    y = s * SS * 0.5
    x0, x1 = s * SS * 0.27, s * SS * 0.73
    d.line([(x0, y), (x1, y)], fill=tuple(colour) + (255,), width=w)


def checkbox(c, s):
    r = max(2, round(s * 0.24))
    frames = []
    for i in range(4):
        frames.append(plate(s, s, r, c["box_fill"][i], c["box_line"][i]))
    for mark in (tick, dash):
        for i in range(4):
            f = plate(s, s, r, c["accent"][i], c["accent"][i])
            mark(ImageDraw.Draw(f), s, c["on_accent"])
            frames.append(f)
    return stack(frames)


def radio(c, s):
    frames = []
    for i in range(4):
        f = canvas(s, s, c["box_line"][i])
        d = ImageDraw.Draw(f)
        d.ellipse((0, 0, s * SS - 1, s * SS - 1), fill=tuple(c["box_line"][i]) + (255,))
        d.ellipse((SS, SS, s * SS - 1 - SS, s * SS - 1 - SS), fill=tuple(c["box_fill"][i]) + (255,))
        frames.append(f)
    for i, dot in enumerate((0.23, 0.27, 0.19, 0.23)):
        f = canvas(s, s, c["accent"][i])
        d = ImageDraw.Draw(f)
        d.ellipse((0, 0, s * SS - 1, s * SS - 1), fill=tuple(c["accent"][i]) + (255,))
        m = s * SS / 2
        rad = s * SS * dot
        d.ellipse((m - rad, m - rad, m + rad, m + rad), fill=tuple(c["on_accent"]) + (255,))
        frames.append(f)
    return stack(frames)


def group_box(c):
    f = canvas(22, 22, c["sides"])
    ImageDraw.Draw(f).rounded_rectangle(box(0, 0, 22, 22), 4 * SS, outline=tuple(c["sides"]) + (255,), width=SS)
    return stack([f])


def combo_box(c):
    fills = (c["field"], c["field_hot"], c["field_hot"], c["field_disabled"])
    frames = []
    for i in range(4):
        frames.append(plate(24, 24, 4, fills[i], c["sides"], c["top"], c["bottom"] if i < 3 else None))
    return stack(frames)


def shade(w, h, r, c, alpha, inset=0):
    f = canvas(w, h, c["tint"])
    if alpha:
        ImageDraw.Draw(f).rounded_rectangle(box(inset, inset, w - inset, h - inset), r * SS,
                                            fill=tuple(c["tint"]) + (alpha,))
    return f


def combo_button(c):
    return stack([shade(18, 18, 3, c, a, 3) for a in (0, c["tint_alpha"][1], c["tint_alpha"][2], 0)])


def chevron(d, w, h, colour):
    """A small V, in the middle of w x h."""
    a = w * SS * 0.5
    y0, y1 = h * SS * 0.5 - 1.5 * SS, h * SS * 0.5 + 1.5 * SS
    half = 3.5 * SS
    line = int(1.1 * SS)
    points = [(a - half, y0), (a, y1), (a + half, y0)]
    d.line(points, fill=tuple(colour) + (255,), width=line, joint="curve")


def combo_chevron(c):
    frames = []
    for i in range(4):
        f = canvas(9, 7, c["glyph"][i])
        chevron(ImageDraw.Draw(f), 9, 7, c["glyph"][i])
        frames.append(f)
    return stack(frames)


def edit(c):
    """Normal, hot, selected, disabled, focused, read-only, assist."""
    frames = []
    for state in range(7):
        disabled = state in (3, 5)
        fill = c["field_disabled"] if disabled else c["field"]
        f = plate(24, 24, 4, fill, c["sides"], c["top"])
        d = ImageDraw.Draw(f)
        if state == 4:
            colour, rows = c["accent"][0], 2
        elif disabled:
            colour, rows = c["sides"], 1
        else:
            colour, rows = c["underline"], 1
        # the line under the text, inside the rounded corners
        d.rectangle(box(3, 24 - rows, 21, 24), fill=tuple(colour) + (255,))
        frames.append(f)
    return stack(frames)


def arrow(d, s, direction, colour):
    m = s * SS * 0.5
    a, b = 3.0 * SS, 2.0 * SS
    if direction == 0:
        points = [(m - a, m + b), (m + a, m + b), (m, m - b)]
    elif direction == 1:
        points = [(m - a, m - b), (m + a, m - b), (m, m + b)]
    elif direction == 2:
        points = [(m + b, m - a), (m + b, m + a), (m - b, m)]
    else:
        points = [(m - b, m - a), (m - b, m + a), (m + b, m)]
    d.polygon(points, fill=tuple(colour) + (255,))


def scroll_arrows(c):
    frames = []
    for direction in range(4):
        for i in range(4):
            f = plate(15, 15, 0, c["track"], c["track"])
            arrow(ImageDraw.Draw(f), 15, direction, c["thumb"][i] if i else c["thumb"][0])
            frames.append(f)
    return stack(frames)


def scroll_shaft(c):
    return stack([plate(16, 16, 0, c["track"], c["track"]) for _ in range(4)])


def scroll_thumb(c, vertical):
    widths = (4, 7, 7, 4)
    frames = []
    w, h = (15, 22) if vertical else (22, 15)
    for i in range(4):
        f = plate(w, h, 0, c["track"], c["track"])
        d = ImageDraw.Draw(f)
        t = widths[i]
        if vertical:
            x = (15 - t) // 2
            d.rounded_rectangle(box(x, 1, x + t, 21), t / 2 * SS, fill=tuple(c["thumb"][i]) + (255,))
        else:
            y = (15 - t) // 2
            d.rounded_rectangle(box(1, y, 21, y + t), t / 2 * SS, fill=tuple(c["thumb"][i]) + (255,))
        frames.append(f)
    return stack(frames)


def slider_track(c, vertical):
    w, h = (4, 8) if vertical else (8, 4)
    f = canvas(w, h, c["slider"])
    ImageDraw.Draw(f).rounded_rectangle(box(0, 0, w, h), 2 * SS, fill=tuple(c["slider"]) + (255,))
    return stack([f])


def track_thumb(c):
    """Normal, hot, pressed, focused, disabled."""
    s = 11
    frames = []
    for accent, dot in ((c["accent"][0], 0.27), (c["accent"][1], 0.33), (c["accent"][2], 0.22),
                        (c["accent"][0], 0.27), (c["accent"][3], 0.27)):
        f = canvas(s, s, c["knob_line"])
        d = ImageDraw.Draw(f)
        d.ellipse((0, 0, s * SS - 1, s * SS - 1), fill=tuple(c["knob_line"]) + (255,))
        d.ellipse((SS, SS, s * SS - 1 - SS, s * SS - 1 - SS), fill=tuple(c["knob"]) + (255,))
        m = s * SS / 2
        rad = s * SS * dot
        d.ellipse((m - rad, m - rad, m + rad, m + rad), fill=tuple(accent) + (255,))
        frames.append(f)
    return stack(frames)


def progress(colour, rounded, vertical=False):
    """The bar as Windows 11 has it, thin: a band in the middle of what is
    stretched to the height of the control."""
    f = canvas(16, 16, colour)
    d = ImageDraw.Draw(f)
    if rounded:
        d.rounded_rectangle(box(0, 6, 16, 11), 2.5 * SS, fill=tuple(colour) + (255,))
    else:
        d.rectangle(box(0, 6, 16, 11), fill=tuple(colour) + (255,))
    if vertical:
        f = f.transpose(Image.ROTATE_90)
    return stack([f])


def over(colour, under, alpha):
    return tuple((a * alpha + b * (255 - alpha)) // 255 for a, b in zip(colour, under))


def tab_item(c):
    """Normal, hot, selected, disabled, focused: rounded at the top, open at
    the bottom. Opaque, on the colour of dialogs: nothing erases behind a tab,
    and a see-through normal state left the hot one showing."""
    frames = []
    for state in range(5):
        f = Image.new("RGBA", (18 * SS, 18 * SS), tuple(c["face"]) + (255,))
        d = ImageDraw.Draw(f)
        if state == 2:
            d.rounded_rectangle((0, 0, 18 * SS - 1, 24 * SS), 4 * SS, fill=tuple(c["sides"]) + (255,))
            d.rounded_rectangle((SS, SS, 17 * SS - 1, 24 * SS), 3 * SS, fill=tuple(c["surface"]) + (255,))
        elif state == 1:
            hot = over(c["tint"], c["face"], c["tint_alpha"][1])
            d.rounded_rectangle((SS, SS, 17 * SS - 1, 24 * SS), 3 * SS, fill=hot + (255,))
        frames.append(f)
    return stack(frames)


def small_chevron(w, h, direction, colour):
    """V pointing up, down, left or right, in the middle of w x h."""
    f = canvas(w, h, colour)
    x, y = w * SS * 0.5, h * SS * 0.5
    a, b = 3.0 * SS, 1.6 * SS
    points = {
        0: [(x - a, y + b), (x, y - b), (x + a, y + b)],
        1: [(x - a, y - b), (x, y + b), (x + a, y - b)],
        2: [(x + b, y - a), (x - b, y), (x + b, y + a)],
        3: [(x - b, y - a), (x + b, y), (x - b, y + a)],
    }[direction]
    ImageDraw.Draw(f).line(points, fill=tuple(colour) + (255,), width=int(1.15 * SS), joint="curve")
    return f


def tree_glyph(c):
    """Closed, open: chevrons, not the boxes of old."""
    return stack([small_chevron(12, 12, 3, c["glyph"][0]), small_chevron(12, 12, 1, c["glyph"][0])])


def spin_button(c):
    return stack([shade(15, 16, 3, c, a, 1) for a in (0, c["tint_alpha"][1], c["tint_alpha"][2], 0)])


def spin_glyph(c, direction):
    w, h = (9, 7) if direction < 2 else (7, 9)
    return stack([small_chevron(w, h, direction, c["glyph"][i]) for i in range(4)])


def tab_pane(c):
    return stack([plate(24, 24, 4, c["surface"], c["sides"])])


def toolbar_button(c):
    return stack([shade(20, 20, 4, c, a, 1) for a in c["tint_alpha"]])


def pictures(c, out):
    p = c["prefix"]
    made = []
    for name, im in (
        ("BUTTON", button(c)),
        ("CHECKBOX13", checkbox(c, 13)), ("CHECKBOX16", checkbox(c, 16)), ("CHECKBOX25", checkbox(c, 25)),
        ("RADIOBUTTON13", radio(c, 13)), ("RADIOBUTTON16", radio(c, 16)), ("RADIOBUTTON25", radio(c, 25)),
        ("GROUPBOX", group_box(c)),
        ("COMBOBOX", combo_box(c)), ("COMBOBUTTON", combo_button(c)), ("COMBOCHEVRON", combo_chevron(c)),
        ("EDIT", edit(c)),
        ("SCROLLARROWS", scroll_arrows(c)), ("SCROLLSHAFT", scroll_shaft(c)),
        ("SCROLLTHUMBVERTICAL", scroll_thumb(c, True)), ("SCROLLTHUMBHORIZONTAL", scroll_thumb(c, False)),
        ("SLIDERTRACK", slider_track(c, False)), ("SLIDERTRACKVERT", slider_track(c, True)),
        ("TRACKTHUMB", track_thumb(c)),
        ("PROGRESSTRACK", progress(c["progress_track"], True)), ("PROGRESSCHUNK", progress(c["accent"][0], False)),
        ("PROGRESSTRACKVERT", progress(c["progress_track"], True, True)),
        ("PROGRESSCHUNKVERT", progress(c["accent"][0], False, True)),
        ("TABITEM", tab_item(c)), ("TABPANE", tab_pane(c)),
        ("TOOLBARBUTTON", toolbar_button(c)),
        ("TREEGLYPH", tree_glyph(c)),
        ("SPINBUTTON", spin_button(c)),
        ("SPINUP", spin_glyph(c, 0)), ("SPINDOWN", spin_glyph(c, 1)), ("SPINLEFT", spin_glyph(c, 2)), ("SPINRIGHT", spin_glyph(c, 3)),
    ):
        made.append(save(im, out, "%s_%s" % (p, name)))
    return made


# --- the INI files ---

def rgb(colour):
    return "%d %d %d" % tuple(colour)


def overrides(c):
    """{section: {key: value, or None to take the key away}}"""
    p = c["prefix"]
    drop_key = {"Transparent": None, "TransparentColor": None}
    img = lambda name: "%s\\%s.bmp" % (p, name)
    out = {
        "Button.Pushbutton": dict(drop_key, ImageFile=img("Button"), SizingMargins="6, 6, 6, 6", TextColor=rgb(c["text"])),
        "Button.Pushbutton(Hot)": {"TextColor": rgb(c["text"])},
        "Button.Pushbutton(Normal)": {"TextColor": rgb(c["text"])},
        "Button.Pushbutton(Pressed)": {"TextColor": rgb(c["text_pressed"])},
        "Button.Pushbutton(Disabled)": {"TextColor": rgb(c["text_disabled"])},
        "Button.Checkbox": {"ImageFile1": img("CheckBox13"), "ImageFile2": img("CheckBox16"), "ImageFile3": img("CheckBox25")},
        "Button.Radiobutton": dict(drop_key, ImageFile1=img("RadioButton13"), ImageFile2=img("RadioButton16"),
                                   ImageFile3=img("RadioButton25")),
        "Button.Groupbox": dict(drop_key, ImageFile=img("GroupBox"), SizingMargins="5, 5, 5, 5", SizingType="Stretch",
                                TextColor=rgb(c["text"])),
        "Combobox": {"BgType": "ImageFile", "ImageFile": img("ComboBox"), "ImageCount": "4", "ImageLayout": "vertical",
                     "SizingMargins": "6, 6, 6, 6", "SizingType": "Stretch", "BorderColor": None, "BorderSize": None},
        "Combobox.DropDownButton": dict(drop_key, ImageFile=img("ComboButton"), GlyphImageFile=img("ComboChevron"),
                                        SizingMargins="4, 4, 4, 4"),
        "Edit.EditText": {"BgType": "ImageFile", "ImageFile": img("Edit"), "ImageCount": "7", "ImageLayout": "vertical",
                          "SizingMargins": "5, 5, 5, 5", "SizingType": "Stretch"},
        "ScrollBar.ArrowBtn": {"ImageFile": img("ScrollArrows"), "ImageFile1": None, "ImageFile2": None,
                               "ImageSelectType": None, "MinSize1": None, "MinSize2": None, "NormalSize": None,
                               "GlyphType": "None", "GlyphTransparent": None, "TrueSizeScalingType": None,
                               "TrueSizeStretchMark": None, "SourceShrink": None, "UniformSizing": None,
                               "SizingMargins": "0, 0, 0, 0"},
        "ScrollBar.ThumbBtnVert": {"ImageFile": img("ScrollThumbVertical"), "SizingMargins": "0, 0, 6, 6"},
        "ScrollBar.ThumbBtnHorz": {"ImageFile": img("ScrollThumbHorizontal"), "SizingMargins": "6, 6, 0, 0"},
        "TrackBar.Track": {"ImageFile": img("SliderTrack"), "SizingMargins": "2, 2, 1, 1"},
        "TrackBar.TrackVert": {"ImageFile": img("SliderTrackVert"), "SizingMargins": "1, 1, 2, 2"},
        "Progress": {"ProgressChunkSize": "1", "ProgressSpaceSize": "0"},
        "Progress.Bar": {"ImageFile": img("ProgressTrack"), "MinSize": None, "SizingMargins": "3, 3, 0, 0"},
        "Progress.BarVert": {"ImageFile": img("ProgressTrackVert"), "SizingMargins": "0, 0, 3, 3"},
        "Progress.Chunk": dict(drop_key, ImageFile=img("ProgressChunk"), SizingMargins="0, 0, 0, 0"),
        "Progress.ChunkVert": dict(drop_key, ImageFile=img("ProgressChunkVert"), SizingMargins="0, 0, 0, 0"),
        "TreeView.Glyph": {"ImageFile": img("TreeGlyph")},
        "Tab.Pane": {"ImageFile": img("TabPane"), "SizingMargins": "6, 6, 6, 6"},
    }
    for name in ("ScrollBar.GripperHorz", "ScrollBar.GripperVert"):
        out[name] = {"BgType": "None", "ImageFile": None, "ImageCount": None, "ImageLayout": None, "SizingType": None,
                     "Transparent": None}
    for name in ("ScrollBar.LowerTrackHorz", "ScrollBar.UpperTrackHorz", "ScrollBar.LowerTrackVert", "ScrollBar.UpperTrackVert"):
        out[name] = {"ImageFile": img("ScrollShaft"), "SizingMargins": "0, 0, 0, 0"}
    for name in ("TrackBar.Thumb", "TrackBar.ThumbVert", "TrackBar.ThumbBottom", "TrackBar.ThumbTop", "TrackBar.ThumbLeft",
                 "TrackBar.ThumbRight"):
        out[name] = dict(drop_key, ImageFile=img("TrackThumb"), ImageFile1=None, ImageFile2=None, ImageFile3=None,
                         SizingType="TrueSize", SizingMargins=None, TrueSizeStretchMark=None, UniformSizing=None)
    for name in ("TabItem", "TabItemBothEdge", "TabItemLeftEdge", "TabItemRightEdge", "TopTabItem", "TopTabItemBothEdge",
                 "TopTabItemLeftEdge", "TopTabItemRightEdge"):
        # comctl32 gives a tab this much more room (ReactOS patch 0103)
        out["Tab." + name] = dict(drop_key, ImageFile=img("TabItem"), TextColor=rgb(c["text"]), ContentMargins="9, 9, 2, 0")
    for name, glyph in (("Up", "SpinUp"), ("Down", "SpinDown"), ("Uphorz", "SpinRight"), ("Downhorz", "SpinLeft")):
        out["Spin." + name] = dict(drop_key, ImageFile=img("SpinButton"), GlyphImageFile=img(glyph),
                                   SizingMargins="3, 3, 3, 3", SizingType="Stretch")
    for name in ("Button", "DropdownButton", "SplitButton", "SplitButtonDropdown"):
        out["Toolbar." + name] = dict(drop_key, ImageFile=img("ToolbarButton"), SizingMargins="5, 5, 5, 5")
    return out


KEY = re.compile(r"^\s*([A-Za-z0-9]+)\s*=")


def make_ini(src, c, out):
    with open(os.path.join(src, "textfiles", c["base"]), encoding="utf-8-sig", errors="replace") as f:
        lines = f.read().splitlines()
    todo = {k.lower(): (k, v) for k, v in overrides(c).items()}
    result = []
    section, pending = None, {}

    def close():
        for key, value in pending.items():
            if value is not None:
                result.append("%s = %s" % (key, value))

    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            close()
            section = stripped[1:-1]
            pending = dict(todo.pop(section.lower(), (None, {}))[1])
            result.append(line)
            continue
        m = KEY.match(line)
        if m and pending:
            name = next((k for k in pending if k.lower() == m.group(1).lower()), None)
            if name is not None:
                value = pending.pop(name)
                if value is not None:
                    result.append("%s = %s" % (m.group(1), value))
                continue
        result.append(line)
    close()
    # the parts Mizu has no section of
    for name, values in todo.values():
        result.append("")
        result.append("[%s]" % name)
        for key, value in values.items():
            if value is not None:
                result.append("%s = %s" % (key, value))
    text = "\n".join(result) + "\n"
    text = text.replace("Source Sans Pro", "Inter")
    text = re.sub(r"(?mi)^(\s*CaptionFont\s*=\s*Inter,\s*)10\b", r"\g<1>9", text)
    with open(os.path.join(out, "textfiles", c["target"]), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(text)


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.join(out, "bitmaps"), exist_ok=True)
    os.makedirs(os.path.join(out, "textfiles"), exist_ok=True)
    for c in (LIGHT, DARK):
        names = pictures(c, out)
        make_ini(src, c, out)
        print("%d pictures of %s, %s" % (len(names), c["prefix"], c["target"]))
        for name in names:
            print('%s_BMP BITMAP "bitmaps/%s.bmp"' % (name, name))


if __name__ == "__main__":
    main()
