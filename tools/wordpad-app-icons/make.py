#!/usr/bin/env python3
"""The icons of Arctic WordPad and of its documents, from the We10X theme
(https://github.com/yeyushengfan258/We10X-icon-theme, GPL-3.0): the program
is its ms-word.svg, a DOCX its application-vnd.ms-word.svg, an RTF its
text-richtext.svg. The SVGs are kept beside this script.

Needs rsvg-convert (librsvg2-bin; the arctic-ros distro has it):

    python3 tools/wordpad-app-icons/make.py
"""
import importlib.util
import os
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "runtime/wine/modules/programs/wordpad")

spec = importlib.util.spec_from_file_location("we10x", os.path.join(ROOT, "tools/make-we10x-icons.py"))
we10x = importlib.util.module_from_spec(spec)
spec.loader.exec_module(we10x)

ICONS = (("ms-word.svg", "wordpad.ico"), ("application-vnd.ms-word.svg", "document.ico"),
         ("text-richtext.svg", "rtf.ico"))

with tempfile.TemporaryDirectory() as work:
    for svg, ico in ICONS:
        we10x.make_ico(work, os.path.join(OUT, ico), os.path.join(HERE, svg), ico[:-4])
        print(os.path.join(OUT, ico))
