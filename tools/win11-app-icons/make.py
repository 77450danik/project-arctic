#!/usr/bin/env python3
"""The icons of the programs Windows 11 shows in its own way, from the Win11
icon theme (https://github.com/yeyushengfan258/Win11-icon-theme, GPL-3.0),
Windows 11's look drawn by its author. The SVGs are kept beside this script:

    paint.svg     gnome-paint.svg               Paint (a palette and a brush)
    photos.svg    eog.svg                       Photos (hills under the sun)
    explorer.svg  file-manager.svg              File Explorer (the yellow folder)
    taskmgr.svg   utilities-system-monitor.svg  Task Manager (a graph)

Needs rsvg-convert (librsvg2-bin; the arctic-ros distro has it):

    python3 tools/win11-app-icons/make.py
"""
import importlib.util
import os
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

spec = importlib.util.spec_from_file_location("we10x", os.path.join(ROOT, "tools/make-we10x-icons.py"))
we10x = importlib.util.module_from_spec(spec)
spec.loader.exec_module(we10x)

ICONS = (
    ("paint.svg", "runtime/wine/modules/programs/mspaint/mspaint.ico"),
    ("photos.svg", "runtime/wine/modules/programs/photos/photos.ico"),
    ("taskmgr.svg", "runtime/wine/modules/programs/taskmgr/taskmgr.ico"),
    # explorer.exe's IDI_MAIN (ReactOS, built with runtime/reactos/files)
    ("explorer.svg", "runtime/reactos/files/base/shell/explorer/res/ico/100.ico"),
)

with tempfile.TemporaryDirectory() as work:
    for svg, ico in ICONS:
        out = os.path.join(ROOT, ico)
        we10x.make_ico(work, out, os.path.join(HERE, svg), os.path.basename(ico)[:-4])
        print(out)
