"""Prints the string tables, menus and dialog texts of a PE file or .mui, to
read the exact wording Windows uses.

usage: python tools/dump-res.py FILE [FILE ...]   (pefile from out/pylib)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "out", "pylib"))
import pefile  # noqa: E402

RT_MENU, RT_DIALOG, RT_STRING = 4, 5, 6
sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def wstr(data, pos):
    end = pos
    while data[end:end + 2] != b"\0\0":
        end += 2
    return data[pos:end].decode("utf-16-le"), end + 2


def menu_texts(data):
    """the texts of a MENU or MENUEX, in order"""
    out = []
    version = struct.unpack_from("<H", data, 0)[0]
    if version == 1:  # MENUEX
        pos = 4 + struct.unpack_from("<H", data, 2)[0]
        while pos + 14 <= len(data):
            pos += 12
            text, pos = wstr(data, pos + 2)
            pos = (pos + 3) & ~3
            if text:
                out.append(text)
    else:
        pos = 4
        while pos + 4 <= len(data):
            flags = struct.unpack_from("<H", data, pos)[0]
            pos += 2 if flags & 0x10 else 4
            text, pos = wstr(data, pos)
            if text:
                out.append(text)
    return out


def utf16_runs(data, minimum=2):
    """readable UTF-16 runs, for dialogs"""
    out, run = [], ""
    for i in range(0, len(data) - 1, 2):
        ch = data[i:i + 2].decode("utf-16-le", errors="ignore")
        if ch and (ch.isprintable() and ord(ch) >= 32):
            run += ch
        else:
            if len(run) >= minimum:
                out.append(run)
            run = ""
    return out


for path in sys.argv[1:]:
    pe = pefile.PE(path)
    print("=====", path)
    for rtype in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if rtype.id not in (RT_MENU, RT_DIALOG, RT_STRING):
            continue
        for rname in rtype.directory.entries:
            for rlang in rname.directory.entries:
                entry = rlang.data.struct
                data = pe.get_data(entry.OffsetToData, entry.Size)
                if rtype.id == RT_STRING:
                    pos = 0
                    for i in range(16):
                        n = struct.unpack_from("<H", data, pos)[0]
                        pos += 2
                        if n:
                            text = data[pos:pos + 2 * n].decode("utf-16-le")
                            print("  string %d: %r" % ((rname.id - 1) * 16 + i, text))
                        pos += 2 * n
                elif rtype.id == RT_MENU:
                    print("  menu %s: %s" % (rname.id, menu_texts(data)))
                else:
                    print("  dialog %s: %s" % (rname.id, utf16_runs(data)))
