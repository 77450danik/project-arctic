"""Prints a key of a Windows registry hive file (regf: NTUSER.DAT, SOFTWARE...)
with everything under it as .reg text, to take Windows' own data into
runtime/registry (the sound events and schemes of AppEvents, say).

usage: python tools/hive2reg.py HIVE "Key\\Path" [ROOT]
       ROOT is what the hive is in the output, HKEY_CURRENT_USER by default
"""
import struct
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


class Hive:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        if self.data[:4] != b"regf":
            raise SystemExit("not a registry hive")
        self.root = struct.unpack_from("<I", self.data, 0x24)[0]

    def cell(self, offset):
        pos = 0x1000 + offset
        size = -struct.unpack_from("<i", self.data, pos)[0]
        return self.data[pos + 4:pos + size]

    def key(self, offset):
        c = self.cell(offset)
        if c[:2] != b"nk":
            return None
        flags, = struct.unpack_from("<H", c, 2)
        nsub, = struct.unpack_from("<I", c, 0x14)
        sublist, = struct.unpack_from("<I", c, 0x1c)
        nval, vallist = struct.unpack_from("<II", c, 0x24)
        namelen, = struct.unpack_from("<H", c, 0x48)
        raw = c[0x4c:0x4c + namelen]
        name = raw.decode("latin-1") if flags & 0x20 else raw.decode("utf-16-le")
        return {"name": name, "nsub": nsub, "sublist": sublist, "nval": nval, "vallist": vallist}

    def subkeys(self, key):
        if not key["nsub"] or key["sublist"] == 0xFFFFFFFF:
            return []
        return [self.key(o) for o in self._list(key["sublist"])]

    def _list(self, offset):
        c = self.cell(offset)
        sig, count = c[:2], struct.unpack_from("<H", c, 2)[0]
        out = []
        if sig in (b"lf", b"lh"):
            for i in range(count):
                out.append(struct.unpack_from("<I", c, 4 + i * 8)[0])
        elif sig == b"li":
            for i in range(count):
                out.append(struct.unpack_from("<I", c, 4 + i * 4)[0])
        elif sig == b"ri":
            for i in range(count):
                out += self._list(struct.unpack_from("<I", c, 4 + i * 4)[0])
        return out

    def values(self, key):
        out = []
        if not key["nval"] or key["vallist"] == 0xFFFFFFFF:
            return out
        lst = self.cell(key["vallist"])
        for i in range(key["nval"]):
            c = self.cell(struct.unpack_from("<I", lst, i * 4)[0])
            if c[:2] != b"vk":
                continue
            namelen, size, dataoff, typ, flags = struct.unpack_from("<HIIIH", c, 2)
            raw = c[0x14:0x14 + namelen]
            name = raw.decode("latin-1") if flags & 1 else raw.decode("utf-16-le")
            if size & 0x80000000:
                data = struct.pack("<I", dataoff)[:size & 0x7FFFFFFF]
            else:
                data = self.cell(dataoff)[:size]
            out.append((name, typ, data))
        return out

    def find(self, path):
        key = self.key(self.root)
        for part in [p for p in path.split("\\") if p]:
            for sub in self.subkeys(key):
                if sub and sub["name"].lower() == part.lower():
                    key = sub
                    break
            else:
                return None
        return key


def quote(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def dump(hive, key, path):
    print(f"[{path}]")
    for name, typ, data in hive.values(key):
        left = "@" if not name else f'"{quote(name)}"'
        if typ == 1:
            text = data.decode("utf-16-le", "replace").rstrip("\0")
            print(f'{left}="{quote(text)}"')
        elif typ == 4 and len(data) >= 4:
            print(f"{left}=dword:{struct.unpack('<I', data[:4])[0]:08x}")
        else:
            prefix = "hex" if typ == 3 else f"hex({typ:x})"
            print(f"{left}={prefix}:" + ",".join(f"{b:02x}" for b in data))
    print()
    for sub in hive.subkeys(key):
        if sub:
            dump(hive, sub, path + "\\" + sub["name"])


def main():
    hive = Hive(sys.argv[1])
    path = sys.argv[2]
    root = sys.argv[3] if len(sys.argv) > 3 else "HKEY_CURRENT_USER"
    key = hive.find(path)
    if not key:
        raise SystemExit(f"no {path} in the hive")
    print("Windows Registry Editor Version 5.00\n")
    dump(hive, key, root + "\\" + path.strip("\\"))


if __name__ == "__main__":
    main()
