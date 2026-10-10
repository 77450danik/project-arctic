"""Writes the dialogs and string tables of a PE file or .mui as .rc source:
DIALOGEX with every control's class, id, style and place, STRINGTABLE, to
build Windows' own layouts again in Arctic's modules.

usage: python tools/res2rc.py FILE [--dialog ID ...] [--strings] [--list]
       (pefile from out/pylib)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "out", "pylib"))
import pefile  # noqa: E402

RT_DIALOG, RT_STRING = 5, 6
sys.stdout.reconfigure(encoding="utf-8", errors="replace")

CLASSES = {0x80: "Button", 0x81: "Edit", 0x82: "Static", 0x83: "ListBox", 0x84: "ScrollBar", 0x85: "ComboBox"}


def wstr(data, pos):
    end = pos
    while data[end:end + 2] != b"\0\0":
        end += 2
    return data[pos:end].decode("utf-16-le"), end + 2


def sz_or_ord(data, pos):
    """a name or an ordinal, as dialog templates hold them"""
    first = struct.unpack_from("<H", data, pos)[0]
    if first == 0:
        return None, pos + 2
    if first == 0xFFFF:
        return struct.unpack_from("<H", data, pos + 2)[0], pos + 4
    return wstr(data, pos)


def quote(text):
    if text is None:
        return '""'
    if isinstance(text, int):
        return str(text)
    return '"' + text.replace('"', '""').replace("\n", "\\n").replace("\t", "\\t") + '"'


def dialog_rc(name, data):
    out = []
    signature = struct.unpack_from("<HH", data, 0)
    if signature != (1, 0xFFFF):
        return f"// {name}: not a DIALOGEX\n"
    help_id, ex_style, style, count, x, y, cx, cy = struct.unpack_from("<IIIHhhhh", data, 4)
    pos = 26
    menu, pos = sz_or_ord(data, pos)
    cls, pos = sz_or_ord(data, pos)
    title, pos = wstr(data, pos)
    font = None
    if style & 0x40 or style & 0x48:  # DS_SETFONT / DS_SHELLFONT
        size, weight, italic, charset = struct.unpack_from("<HHBB", data, pos)
        face, pos = wstr(data, pos + 6)
        font = f'{size}, "{face}", {weight}, {italic}, {charset:#x}'
    out.append(f"{name} DIALOGEX {x}, {y}, {cx}, {cy}")
    out.append(f"STYLE {style:#010x}")
    if ex_style:
        out.append(f"EXSTYLE {ex_style:#010x}")
    if title:
        out.append(f"CAPTION {quote(title)}")
    if menu is not None:
        out.append(f"MENU {quote(menu)}")
    if cls is not None:
        out.append(f"CLASS {quote(cls)}")
    if font:
        out.append(f"FONT {font}")
    out.append("BEGIN")
    for _ in range(count):
        pos = (pos + 3) & ~3
        c_help, c_ex, c_style, cx_, cy_, cw, ch, cid = struct.unpack_from("<IIIhhhhI", data, pos)
        pos += 24
        c_cls, pos = sz_or_ord(data, pos)
        c_text, pos = sz_or_ord(data, pos)
        extra = struct.unpack_from("<H", data, pos)[0]
        pos += 2 + extra
        if isinstance(c_cls, int):
            c_cls = CLASSES.get(c_cls, f"#{c_cls}")
        cid = cid if cid < 0x80000000 else cid - 0x100000000
        line = f"    CONTROL {quote(c_text)}, {cid}, {quote(c_cls)}, {c_style:#010x}, {cx_}, {cy_}, {cw}, {ch}"
        if c_ex or c_help:
            line += f", {c_ex:#010x}"
        if c_help:
            line += f", {c_help}"
        out.append(line)
    out.append("END")
    return "\n".join(out) + "\n"


def strings_rc(pe):
    out = ["STRINGTABLE", "BEGIN"]
    for entry in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if entry.id != RT_STRING:
            continue
        for block in entry.directory.entries:
            for lang in block.directory.entries:
                rva, size = lang.data.struct.OffsetToData, lang.data.struct.Size
                data = pe.get_memory_mapped_image()[rva:rva + size]
                pos = 0
                for i in range(16):
                    length = struct.unpack_from("<H", data, pos)[0]
                    text = data[pos + 2:pos + 2 + length * 2].decode("utf-16-le")
                    pos += 2 + length * 2
                    if text:
                        out.append(f"    {(block.id - 1) * 16 + i}, {quote(text)}")
    out.append("END")
    return "\n".join(out) + "\n"


def main():
    args = sys.argv[1:]
    path = args.pop(0)
    want = set()
    strings = listing = False
    while args:
        a = args.pop(0)
        if a == "--dialog":
            while args and not args[0].startswith("--"):
                want.add(args.pop(0))
        elif a == "--strings":
            strings = True
        elif a == "--list":
            listing = True
    pe = pefile.PE(path)
    for entry in pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if entry.id != RT_DIALOG:
            continue
        for dlg in entry.directory.entries:
            name = str(dlg.id) if dlg.id is not None else str(dlg.name)
            for lang in dlg.directory.entries:
                rva, size = lang.data.struct.OffsetToData, lang.data.struct.Size
                data = pe.get_memory_mapped_image()[rva:rva + size]
                if listing:
                    title = ""
                    if struct.unpack_from("<HH", data, 0) == (1, 0xFFFF):
                        p = 26
                        _, p = sz_or_ord(data, p)
                        _, p = sz_or_ord(data, p)
                        title, p = wstr(data, p)
                    print(f"{name}: {title!r} ({struct.unpack_from('<H', data, 16)[0]} controls)")
                elif not want or name in want:
                    print(dialog_rc(name, data))
    if strings:
        print(strings_rc(pe))


if __name__ == "__main__":
    main()
