"""Which imports of a program's PE files no DLL of this system exports.

usage: impcheck.py FILE_OR_DIR...   (checks *.exe, *.dll; normal and delay imports)
DLLs are looked up next to the file, then in System32 (SysWOW64 for 32-bit files; and
WinSxS common-controls v6 for comctl32, as a manifest would). API sets are no files in
Wine: their functions are looked up in the DLLs that host them."""
import os, struct, sys, glob

WINDIR = os.environ.get('SystemRoot', r'C:\Windows')
SYS64 = os.path.join(WINDIR, 'System32')
SYS32 = os.path.join(WINDIR, 'SysWOW64')

class PE:
    def __init__(self, path):
        self.d = d = open(path, 'rb').read()
        e = struct.unpack_from('<I', d, 0x3c)[0]
        if d[e:e + 4] != b'PE\0\0': raise ValueError('not PE')
        nsec = struct.unpack_from('<H', d, e + 6)[0]
        opt = e + 24
        self.pe64 = struct.unpack_from('<H', d, opt)[0] == 0x20b
        self.dd = opt + (112 if self.pe64 else 96)
        sh = opt + struct.unpack_from('<H', d, e + 20)[0]
        self.secs = [struct.unpack_from('<IIII', d, sh + i * 40 + 8) for i in range(nsec)]
        self.base = struct.unpack_from('<Q' if self.pe64 else '<I', d, opt + (24 if self.pe64 else 28))[0]

    def off(self, rva):
        for vs, va, rs, ro in self.secs:
            if va <= rva < va + max(vs, rs): return ro + rva - va
        raise ValueError(hex(rva))

    def cstr(self, rva):
        o = self.off(rva); return self.d[o:self.d.index(b'\0', o)].decode('latin1')

    def dirent(self, i):
        return struct.unpack_from('<II', self.d, self.dd + 8 * i)

    def exports(self):
        rva, size = self.dirent(0)
        if not rva: return set(), set()
        x = self.off(rva)
        _, base, nfunc, nnames, afunc, anames, aords = struct.unpack_from('<7I', self.d, x + 12)
        ords = {base + i for i in range(nfunc) if struct.unpack_from('<I', self.d, self.off(afunc) + 4 * i)[0]}
        names = {self.cstr(struct.unpack_from('<I', self.d, self.off(anames) + 4 * i)[0]) for i in range(nnames)}
        return names, ords

    def _thunks(self, rva):
        size = 8 if self.pe64 else 4
        flag = 1 << (63 if self.pe64 else 31)
        o = self.off(rva)
        while True:
            v = struct.unpack_from('<Q' if self.pe64 else '<I', self.d, o)[0]
            if not v: return
            if v & flag: yield ('#', v & 0xffff)
            else: yield ('n', self.cstr((v & 0x7fffffff) + 2))
            o += size

    def imports(self):
        out = []
        rva, size = self.dirent(1)
        if rva:
            o = self.off(rva)
            while True:
                oft, _, _, name, ft = struct.unpack_from('<5I', self.d, o)
                if not name: break
                out.append((self.cstr(name), list(self._thunks(oft or ft)), False))
                o += 20
        rva, size = self.dirent(13)
        if rva:
            o = self.off(rva)
            while True:
                attrs, name, hmod, iat, int_, *_ = struct.unpack_from('<8I', self.d, o)
                if not name: break
                fix = (lambda r: r) if attrs & 1 else (lambda r: r - self.base)
                out.append((self.cstr(fix(name)), list(self._thunks(fix(int_))), True))
                o += 32
        return out

cache = {}
def dll_exports(name, here, pe64=True):
    key = (name.lower(), here, pe64)
    if key in cache: return cache[key]
    cands = [os.path.join(here, name), os.path.join(SYS64 if pe64 else SYS32, name)]
    if name.lower() == 'comctl32.dll':
        arch = 'amd64' if pe64 else 'x86'
        cands[1:1] = glob.glob(os.path.join(WINDIR, 'WinSxS', arch + '_microsoft.windows.common-controls_*', 'comctl32.dll'))
    res = None
    for c in cands:
        if os.path.isfile(c):
            try: res = (c, PE(c).exports())
            except Exception: res = (c, (set(), set()))
            break
    cache[key] = res
    return res

# API sets are no files: the loader maps them to these hosts
API_HOSTS = ['kernelbase.dll', 'kernel32.dll', 'ntdll.dll', 'combase.dll', 'shcore.dll', 'powrprof.dll',
             'user32.dll', 'advapi32.dll', 'ucrtbase.dll', 'sechost.dll', 'gdi32.dll', 'shell32.dll',
             'shlwapi.dll', 'ole32.dll', 'oleaut32.dll', 'rpcrt4.dll', 'bcrypt.dll', 'crypt32.dll',
             'winmm.dll', 'version.dll', 'setupapi.dll', 'cfgmgr32.dll', 'dwmapi.dll', 'uxtheme.dll', 'msvcrt.dll']

def api_set_exports(here, pe64):
    names, ords = set(), set()
    for h in API_HOSTS:
        ex = dll_exports(h, here, pe64)
        if ex: names |= ex[1][0]
    return 'API set hosts', (names, ords)

def check(path):
    try: pe = PE(path)
    except Exception: return
    here = os.path.dirname(path)
    for dll, thunks, delay in pe.imports():
        lower = dll.lower()
        ex = api_set_exports(here, pe.pe64) if lower.startswith(('api-ms-', 'ext-ms-')) else dll_exports(dll, here, pe.pe64)
        tag = ' (delay)' if delay else ''
        if not ex:
            print(f'{os.path.basename(path)}: {dll}{tag}: NO SUCH DLL ({len(thunks)} imports)')
            continue
        names, ords = ex[1]
        miss = [f'#{v}' if k == '#' else v for k, v in thunks if (k == '#' and v not in ords) or (k == 'n' and v not in names)]
        if miss:
            print(f'{os.path.basename(path)}: {dll}{tag} [{ex[0]}]: {", ".join(miss)}')

for a in sys.argv[1:]:
    files = [a] if os.path.isfile(a) else [f for f in glob.glob(os.path.join(a, '*')) if f.lower().endswith(('.exe', '.dll'))]
    for f in files: check(f)
