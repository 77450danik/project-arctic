#!/usr/bin/env python3
"""The We10X icon theme as Arctic's: .ico files named after the icon groups
of the modules they stand for (runtime/icons/We10X/<module>/<id>.ico: shell32,
imageres and the Control Panel's applets), which user32 takes instead of the
modules' own when the theme is chosen (HKCU\\Software\\Arctic\\Icons\\Theme,
Personalization). The folders of the user are We10X's "special" colours, the
Windows-like ones.

We10X: https://github.com/yeyushengfan258/We10X-icon-theme (GPL-3.0).
The folders of the user, the plain folder and This PC are the same author's
Windows 11 ones instead, as colourful as Windows 11's own:
https://github.com/yeyushengfan258/Win11-icon-theme (GPL-3.0).
Needs rsvg-convert (librsvg2-bin). Run where the themes are unpacked:

    python3 tools/make-we10x-icons.py <We10X-icon-theme-master> runtime/icons/We10X <Win11-icon-theme>

With "-" for We10X the icons made before stay, and only the Windows 11 ones
are written again.
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 256)

# a module's icon group -> what We10X has for it, the first found wins
SHELL32 = {
    1: ["mimes/application-blank", "mimes/unknown"],         # a document
    2: ["mimes/text-richtext"],
    3: ["mimes/application-x-executable", "mimes/application-x-ms-dos-executable"],
    4: ["special/folder", "places/folder"],
    5: ["special/folder-open", "places/folder-open"],
    6: ["devices/media-flash", "devices/drive-removable-media"],
    7: ["devices/media-flash", "devices/drive-removable-media"],
    8: ["devices/drive-removable-media-usb", "devices/drive-removable-media"],
    9: ["devices/drive-harddisk"],
    10: ["places/network-server", "devices/drive-harddisk"],
    11: ["places/network-server", "devices/drive-harddisk"],
    12: ["devices/drive-optical"],
    13: ["devices/drive-multidisk", "devices/drive-harddisk"],
    14: ["apps/network-workgroup", "places/network-server"],
    15: ["apps/network-workgroup", "places/network-server"],
    16: ["devices/computer"],
    17: ["devices/printer"],
    18: ["apps/network-workgroup"],
    19: ["apps/network-workgroup"],
    21: ["places/folder-recent", "places/folder-documents"],
    22: ["apps/preferences-system", "apps/systemsettings"],
    23: ["apps/preferences-system-search", "apps/system-search"],
    24: ["apps/help-browser"],
    25: ["apps/system-run", "apps/utilities-terminal"],
    32: ["places/user-trash"],
    33: ["places/user-trash-full"],
    35: ["special/user-desktop", "places/user-desktop"],
    36: ["apps/preferences-system", "apps/systemsettings"],
    38: ["devices/printer"],
    39: ["places/folder-fonts", "apps/preferences-desktop-font", "places/folder"],
    41: ["devices/media-optical-audio", "devices/media-optical"],
    44: ["special/folder-bookmark", "places/folder-bookmark"],
    133: ["mimes/application-blank", "mimes/unknown"],
    137: ["apps/preferences-system", "apps/systemsettings"],
    138: ["devices/printer"],
    151: ["mimes/text-x-install", "mimes/text-x-generic"],
    152: ["mimes/text-x-generic"],
    153: ["mimes/application-x-shellscript", "mimes/text-x-script"],
    154: ["mimes/application-x-sharedlib", "mimes/application-x-executable"],
    155: ["mimes/font-x-generic"],
    156: ["mimes/font-x-generic"],
    157: ["mimes/font-x-generic"],
    168: ["devices/printer"],
    172: ["apps/network-workgroup"],
    173: ["special/folder-bookmark", "places/folder-bookmark"],
    175: ["apps/preferences-system-network", "devices/network-card"],
    181: ["special/user-desktop", "places/user-desktop"],
    191: ["places/user-trash"],
    192: ["places/user-trash-full"],
    220: ["apps/preferences-desktop-user", "apps/cs-user-accounts"],
    222: ["devices/drive-optical"],
    224: ["mimes/video-x-generic"],
    225: ["mimes/audio-x-generic"],
    226: ["mimes/image-x-generic"],
    227: ["mimes/video-x-generic"],
    228: ["devices/media-optical-audio", "devices/media-optical"],
    229: ["devices/input-mouse"],
    233: ["devices/media-flash", "devices/drive-removable-media"],
    235: ["special/folder-documents", "places/folder-documents"],
    236: ["special/folder-images", "places/folder-images"],
    237: ["special/folder-music", "places/folder-music"],
    238: ["special/folder-videos", "places/folder-videos"],
    240: ["places/user-trash-full"],
    245: ["devices/printer"],
    248: ["devices/camera-photo"],
    250: ["devices/video-display", "devices/monitor"],
    254: ["places/user-trash"],
    257: ["apps/preferences-system-network", "devices/network-card"],
    259: ["special/user-home", "places/user-home"],
    267: ["special/folder-public", "places/folder-public"],
    268: ["apps/preferences-desktop-accessibility"],
    269: ["apps/preferences-desktop-user", "apps/cs-user-accounts"],
    274: ["apps/preferences-system", "apps/systemsettings"],
    276: ["apps/preferences-system-time"],
    278: ["apps/utilities-terminal", "apps/terminal"],
    279: ["apps/preferences-desktop-user", "apps/cs-user-accounts"],
    281: ["apps/preferences-system-search", "apps/system-search"],
    283: ["devices/input-keyboard"],
    291: ["devices/drive-optical"],
    293: ["devices/media-optical"],
    294: ["devices/drive-optical"],
    295: ["devices/media-optical-recordable", "devices/media-optical"],
    296: ["devices/media-optical-recordable", "devices/media-optical"],
    297: ["devices/media-optical-dvd", "devices/media-optical"],
    298: ["devices/media-optical-dvd", "devices/media-optical"],
    299: ["devices/multimedia-player"],
    300: ["places/network-server"],
    301: ["places/network-server"],
    302: ["devices/drive-optical"],
    303: ["devices/media-flash"],
    304: ["devices/drive-optical"],
    305: ["devices/media-flash", "devices/drive-removable-media"],
    307: ["devices/media-flash-sd-mmc", "devices/media-flash"],
    308: ["devices/media-flash-smart-media", "devices/media-flash"],
    309: ["devices/camera-photo"],
    310: ["devices/phone", "devices/smartphone"],
    314: ["devices/smartphone"],
    315: ["devices/scanner"],
    317: ["devices/camera-video"],
    318: ["devices/drive-optical"],
    319: ["special/folder", "places/folder"],
    321: ["apps/preferences-system", "apps/systemsettings"],
    322: ["special/folder-bookmark", "places/folder-bookmark"],
    323: ["apps/preferences-system-search", "apps/system-search"],
    324: ["apps/help-browser"],
    327: ["places/folder-recent", "places/folder-documents"],
    328: ["apps/system-run", "apps/utilities-terminal"],
    330: ["apps/preferences-system", "apps/systemsettings"],
    16800: ["special/folder-bookmark", "places/folder-bookmark"],
    16801: ["special/folder-download", "places/folder-download"],
}


SOUND = ["preferences/preferences-desktop-sound", "apps/multimedia-volume-control"]
SPEAKERS = ["apps/multimedia-volume-control", "preferences/preferences-desktop-sound"]
POWER = ["preferences/preferences-system-power", "apps/preferences-power"]
NETWORK = ["preferences/preferences-system-network", "apps/network-workgroup"]
DISPLAY = ["apps/preferences-desktop-display", "preferences/preferences-desktop-display"]
SYSTEM = ["apps/preferences-system", "preferences/applications-system"]

MODULES = {
    "shell32": SHELL32,
    # the shield of User Account Control: IDI_SHIELD of user32 (consent.exe,
    # buttons that ask for the administrator) and imageres' 78, Windows' own
    # number for it; Win11-icon-theme's, as Windows 11 draws its own
    "user32": {32518: ["preferences/preferences-security"]},
    # by Windows' numbers: the Control Panel's own items
    "imageres": {
        51: ["devices/printer"],
        78: ["preferences/preferences-security"],   # the UAC shield (Win11-icon-theme's, see WIN11)
        77: ["apps/fonts", "preferences/preferences-desktop-font"],
        80: SYSTEM,
        87: ["apps/system-software-install"],
        109: DISPLAY,
        114: ["apps/preferences-desktop-theme", "preferences/preferences-desktop-theme-global"],
        149: ["devices/computer"],
        166: ["apps/file-manager", "special/folder"],
        9001: NETWORK,
    },
    # the applets of the Control Panel, and what they show inside
    "appwiz": {1: ["apps/system-software-install"]},
    "desk": {40: DISPLAY, 100: DISPLAY},
    "inetcpl": {100: ["apps/web-browser"], 1313: ["apps/web-browser"]},
    "joy": {100: ["devices/input-gaming", "devices/input-gamepad"]},
    "mmsys": {
        3004: SOUND, 3010: SPEAKERS, 3011: ["devices/audio-headphones"], 3014: ["apps/gnome-sound-recorder"],
        3015: ["devices/audio-headphones"], 3017: ["devices/video-display"], 3018: ["devices/audio-card"],
        3052: ["devices/phone"],
    },
    "notifications": {1: ["apps/preferences-desktop-notification", "preferences/preferences-desktop-notification-bell"]},
    "powercfg": {1: POWER},
    "powercpl": {1: POWER, 2: ["apps/applications-games", "preferences/applications-games"]},
    "sysdm": {50: SYSTEM, 51: SYSTEM, 52: ["apps/remote-desktop"], 53: ["apps/preferences-desktop-user", "apps/users"],
              54: ["apps/preferences-security", "preferences/preferences-security"]},
    "timedate": {1: ["apps/preferences-system-time", "preferences/preferences-system-time"]},
    "usercpl": {1: ["apps/preferences-desktop-user", "apps/users"]},
    "systemcpl": {1: ["devices/computer"]},
    "netcenter": {1: NETWORK, 6: ["devices/computer"], 7: ["apps/web-browser"],
                  8: ["preferences/preferences-system-network-ethernet", "apps/preferences-system-network-ethernet"],
                  28: ["preferences/preferences-system-network-ethernet", "apps/preferences-system-network-ethernet"]},
    "sndvol": {1: SOUND, 2: SPEAKERS, 3: ["devices/audio-headphones"], 5: ["devices/video-display"]},
}

# The cards of Arctic's pages drawn as Windows 11's Settings (displaycpl,
# taskbarcpl, programscpl): the pictures of Windows 11's own settings, from
# the Windows 11 theme. The same files are the modules' own icons too (copied
# next to their sources), so that the Standard theme shows these.
CARDS = {
    "displaycpl": {
        101: "preferences/preferences-desktop-display-color",   # Display
        102: "preferences/preferences-desktop-display-nightcolor",  # brightness
        103: "preferences/preferences-desktop-virtual",          # scale
        104: "preferences/preferences-desktop-display",          # resolution
        105: "apps/panel",                                       # orientation
        106: "apps/xfce4-taskbar",                               # smaller modes
        107: "preferences/preferences-desktop-workspaces",       # multiple displays
        108: "apps/system-software-update",                     # detect
        109: "preferences/preferences-advanced",                 # advanced display
        110: "preferences/preferences-desktop-gaming",           # graphics
        111: "preferences/preferences-desktop-theme-global",     # Personalization
        112: "preferences/preferences-desktop-wallpaper",        # background
        113: "preferences/preferences-desktop-screensaver",      # screen saver
        114: "preferences/preferences-desktop-theme-applications",  # the mode
        115: "preferences/preferences-desktop-color",            # colours
        116: "preferences/preferences-desktop-icons",            # icon theme
        117: "apps/preferences-desktop-display",                 # the desktop
        118: "preferences/preferences-desktop-effects",          # the controls' style
        119: "apps/preferences-system-windows",                  # title bars
        120: "apps/gnome-software",                              # widgets
        121: "apps/preferences-desktop-theme-global",            # the desktop's icons
        122: "apps/preferences-desktop-icons",                   # their size
        123: "preferences/cs-panel",                             # taskbar
        124: "preferences/preferences-desktop-menu-edit",        # Start
        125: "apps/utilities-energy-monitor",                    # power
        126: "apps/preferences-system-time",                     # wait
        127: "preferences/preferences-desktop-screensaver",      # sign in on resume
        128: "preferences/preferences-desktop",                  # settings of a screen saver
    },
    "taskbarcpl": {
        101: "preferences/cs-panel",                             # taskbar
        102: "preferences/preferences-desktop-color",            # appearance
        103: "preferences/preferences-desktop-virtual",          # buttons
        104: "preferences/preferences-desktop-menu-edit",        # Start
        105: "preferences/preferences-desktop-notification-bell",  # notification area
        106: "preferences/preferences-desktop-screensaver",      # lock
        107: "apps/xfce4-taskbar",                               # hide
        108: "apps/preferences-desktop-icons",                   # small buttons
        109: "preferences/preferences-desktop-workspaces",       # combine
        110: "apps/preferences-system-time",                     # clock
        111: "apps/preferences-desktop-display",                 # show desktop
        112: "preferences/preferences-desktop-icons",            # icons
        113: "preferences/preferences-desktop",                  # on top, customize
        114: "preferences/preferences-desktop-effects",          # effect
        115: "preferences/preferences-desktop-theme-global",     # the menu's look
    },
    "programscpl": {
        101: "apps/softwarecenter",                              # programs
        102: "apps/system-software-install",                     # install
        103: "apps/system-software-update",                      # updates
    },
}

for _module, _table in CARDS.items():
    MODULES.setdefault(_module, {}).update({_id: [_name] for _id, _name in _table.items()})


# what the Windows 11 theme draws instead: (module, icon group) -> its drawing
WIN11 = {
    ("shell32", 4): "places/folder",
    ("shell32", 5): "places/folder-open",
    ("shell32", 319): "places/folder",
    ("shell32", 16): "devices/computer",
    ("shell32", 35): "places/user-desktop",
    ("shell32", 181): "places/user-desktop",
    ("shell32", 235): "places/folder-documents",
    ("shell32", 236): "places/folder-images",
    ("shell32", 237): "places/folder-music",
    ("shell32", 238): "places/folder-videos",
    ("shell32", 16801): "places/folder-download",
    ("shell32", 267): "places/folder-public",
    ("imageres", 149): "devices/computer",
    ("imageres", 78): "preferences/preferences-security",
    ("user32", 32518): "preferences/preferences-security",
    ("netcenter", 6): "devices/computer",
    ("systemcpl", 1): "devices/computer",
}


for _module, _table in CARDS.items():
    WIN11.update({(_module, _id): _name for _id, _name in _table.items()})


def find_svg11(win11, name):
    category, base = name.split("/")
    if category == "preferences":
        path = os.path.join(win11, "src", "preferences", "32", base + ".svg")
    else:
        path = os.path.join(win11, "src", category, "scalable", base + ".svg")
    return path if os.path.exists(path) else None


def find_svg(src, name, size):
    """The scalable drawing at every size: the theme's small ones are of
    another, flatter style. special/ is the colours of that name (the
    Windows-like folders), preferences/ the settings' drawings."""
    category, base = name.split("/")
    if category == "special":
        path = os.path.join(os.path.dirname(src), "colors", "color-special", base + ".svg")
    elif category == "preferences":
        path = os.path.join(src, "preferences", "32", base + ".svg")
    else:
        path = os.path.join(src, category, "scalable", base + ".svg")
    return path if os.path.exists(path) else None


def write_ico(path, pngs):
    """An icon of PNG images, as Windows Vista's and later."""
    header = struct.pack("<HHH", 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    entries, data = b"", b""
    for size, png in pngs:
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(png), offset + len(data))
        data += png
    with open(path, "wb") as f:
        f.write(header + entries + data)


def make_ico(work, path, svg, tag):
    pngs = []
    for size in SIZES:
        png = os.path.join(work, "%s-%d.png" % (tag, size))
        subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-o", png, svg], check=True)
        with open(png, "rb") as f:
            pngs.append((size, f.read()))
    write_ico(path, pngs)


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    theme, out = sys.argv[1], sys.argv[2]
    win11 = sys.argv[3] if len(sys.argv) == 4 else None
    work = tempfile.mkdtemp()
    made = missing = 0
    for module, table in sorted(MODULES.items()):
        dest = os.path.join(out, module)
        if theme != "-":
            shutil.rmtree(dest, ignore_errors=True)
        os.makedirs(dest, exist_ok=True)
        for icon_id, names in sorted(table.items()):
            tag = "%s-%d" % (module, icon_id)
            svg11 = find_svg11(win11, WIN11[(module, icon_id)]) if win11 and (module, icon_id) in WIN11 else None
            if svg11:
                make_ico(work, os.path.join(dest, "%d.ico" % icon_id), svg11, tag)
                made += 1
                continue
            if theme == "-":
                continue
            src = os.path.join(theme, "src")
            name = next((n for n in names if find_svg(src, n, 256)), None)
            if not name:
                print("no drawing for %s %d (%s)" % (module, icon_id, ", ".join(names)))
                missing += 1
                continue
            make_ico(work, os.path.join(dest, "%d.ico" % icon_id), find_svg(src, name, 256), tag)
            made += 1
    shutil.rmtree(work)
    if theme != "-":
        for license_name in ("COPYING", "LICENSE"):
            if os.path.exists(os.path.join(theme, license_name)):
                shutil.copy(os.path.join(theme, license_name), os.path.join(out, "COPYING"))
                break
    print("%d icons, %d without a drawing" % (made, missing))


if __name__ == "__main__":
    main()
