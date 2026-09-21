"""Points the ReactOS DLLs at the NT runtime's own modules.

ReactOS keeps the kernelbase half of shlwapi (Path*, Str*, Url*, ...) in its
own kernelbase_ros.dll. Wine has the same functions under the Windows name,
kernelbase.dll, so the import is renamed in place in each DLL's import and
delay-import tables. usage: fix-reactos-imports.py file.dll...
"""
import struct
import sys

RENAME = {b"kernelbase_ros.dll": b"kernelbase.dll"}


def sections(data, pe):
    count, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    table = pe + 24 + opt_size
    for i in range(count):
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, table + i * 40 + 8)
        yield vaddr, max(vsize, rawsize), rawptr


def rva_to_offset(data, pe, rva):
    for vaddr, size, rawptr in sections(data, pe):
        if vaddr <= rva < vaddr + size:
            return rva - vaddr + rawptr
    return None


def fix(path):
    data = bytearray(open(path, "rb").read())
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return 0
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    dirs = pe + 24 + (112 if magic == 0x20B else 96)
    changed = 0
    # (directory index, descriptor size, offset of the name RVA in it)
    for index, desc_size, name_at in ((1, 20, 12), (13, 32, 4)):
        rva, size = struct.unpack_from("<II", data, dirs + index * 8)
        off = rva_to_offset(data, pe, rva) if rva else None
        while off is not None:
            desc = data[off:off + desc_size]
            if len(desc) < desc_size or not any(desc):
                break
            name_off = rva_to_offset(data, pe, struct.unpack_from("<I", desc, name_at)[0])
            if name_off is not None:
                end = data.index(b"\0", name_off)
                name = bytes(data[name_off:end])
                new = RENAME.get(name.lower())
                if new:
                    data[name_off:end] = new + b"\0" * (len(name) - len(new))
                    changed += 1
            off += desc_size
    if changed:
        open(path, "wb").write(data)
    return changed


for path in sys.argv[1:]:
    n = fix(path)
    if n:
        print("%s: %d import(s) renamed" % (path, n))
