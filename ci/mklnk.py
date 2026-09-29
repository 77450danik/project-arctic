#!/usr/bin/env python3
"""Writes a Windows shortcut (.lnk, MS-SHLLINK) to a program on a local drive.

The link carries the target as a local path (LinkInfo), its folder as the
working directory and its icon, which is all Explorer needs to show and
start it. No item ID list: the shell resolves the path itself.

usage: mklnk.py OUTPUT.lnk 'C:\\path\\to\\program.exe' [ARGUMENTS]
"""
import struct
import sys

HAS_LINK_INFO = 0x02
HAS_WORKING_DIR = 0x10
HAS_ARGUMENTS = 0x20
HAS_ICON_LOCATION = 0x40
IS_UNICODE = 0x80
CLSID_SHELL_LINK = bytes.fromhex("0114020000000000c000000000000046")


def string_data(text):
    return struct.pack("<H", len(text)) + text.encode("utf-16-le")


def link_info(target):
    volume_id = struct.pack("<IIII", 0x11, 3, 0, 0x10) + b"\0"  # fixed drive, no label
    base_path = target.encode("mbcs" if sys.platform == "win32" else "cp1252") + b"\0"
    header_size = 0x1C
    volume_offset = header_size
    base_offset = volume_offset + len(volume_id)
    suffix_offset = base_offset + len(base_path)
    size = suffix_offset + 1
    header = struct.pack("<IIIIIII", size, header_size, 1, volume_offset, base_offset, 0, suffix_offset)
    return header + volume_id + base_path + b"\0"


def main():
    output, target = sys.argv[1], sys.argv[2]
    arguments = sys.argv[3] if len(sys.argv) > 3 else ""
    icon_index = 0
    folder = target.rsplit("\\", 1)[0]
    flags = HAS_LINK_INFO | HAS_WORKING_DIR | HAS_ICON_LOCATION | IS_UNICODE | (HAS_ARGUMENTS if arguments else 0)
    header = struct.pack("<I16sII", 0x4C, CLSID_SHELL_LINK, flags, 0x20)
    header += b"\0" * 24                                # creation, access, write times
    header += struct.pack("<IiIHHII", 0, icon_index, 1, 0, 0, 0, 0)  # size, icon, SW_SHOWNORMAL, hotkey
    data = header + link_info(target) + string_data(folder)
    if arguments:
        data += string_data(arguments)
    data += string_data(target) + b"\0\0\0\0"
    with open(output, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    main()
