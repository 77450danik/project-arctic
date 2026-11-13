"""Minidump: exception, faulting module, crashing thread's stack scanned for return addresses into modules."""
import struct, sys

def main(path):
    f = open(path, 'rb')
    d = f.read(32)
    sig, ver, nstreams, dir_rva = struct.unpack_from('<4sIII', d)
    f.seek(dir_rva)
    streams = {}
    for _ in range(nstreams):
        t, size, rva = struct.unpack('<III', f.read(12))
        streams.setdefault(t, (size, rva))

    def rd(rva, n):
        f.seek(rva); return f.read(n)

    def mdstr(rva):
        n, = struct.unpack('<I', rd(rva, 4))
        return rd(rva + 4, n).decode('utf-16le', 'replace')

    mods = []
    if 4 in streams:
        size, rva = streams[4]
        n, = struct.unpack('<I', rd(rva, 4))
        for i in range(n):
            e = rd(rva + 4 + i * 108, 108)
            base, sz, cks, ts, name_rva = struct.unpack_from('<QIIII', e)
            mods.append((base, sz, mdstr(name_rva).split('\\')[-1]))
    def where(a):
        for b, s, n in mods:
            if b <= a < b + s: return f'{n}+{a-b:#x}'
        return None

    mem = []
    if 5 in streams:  # MemoryListStream
        size, rva = streams[5]
        n, = struct.unpack('<I', rd(rva, 4))
        for i in range(n):
            start, dsz, drva = struct.unpack('<QII', rd(rva + 4 + i * 16, 16))
            mem.append((start, dsz, drva))
    if 9 in streams:  # Memory64ListStream
        size, rva = streams[9]
        n, base_rva = struct.unpack('<QQ', rd(rva, 16))
        off = base_rva
        for i in range(n):
            start, dsz = struct.unpack('<QQ', rd(rva + 16 + i * 16, 16))
            mem.append((start, dsz, off)); off += dsz
    def readmem(a, n):
        for s, sz, r in mem:
            if s <= a and a + n <= s + sz: return rd(r + a - s, n)
        return None

    size, rva = streams[6]
    e = rd(rva, size)
    tid, _ = struct.unpack_from('<II', e)
    code, flags, rec, addr, nparams = struct.unpack_from('<IIQQI', e, 8)
    params = struct.unpack_from('<15Q', e, 8 + 32)
    ctx_size, ctx_rva = struct.unpack_from('<II', e, 8 + 152)
    print(f'thread {tid:#x} code {code:#x} at {addr:#x} = {where(addr)} params {[hex(p) for p in params[:nparams]]}')
    if code in (0xc06d007f, 0xc06d007e) and nparams:
        info = readmem(params[0], 72)
        if info:
            sz_dll, = struct.unpack_from('<Q', info, 24)
            by_name, = struct.unpack_from('<I', info, 32)
            name, = struct.unpack_from('<Q', info, 40)
            def cstr(a):
                for n in (64, 32, 16):
                    b = readmem(a, n)
                    if b: return b.split(b'\0')[0].decode('ascii', 'replace')
                return f'?{a:#x}'
            print('delay load:', cstr(sz_dll), cstr(name) if by_name else f'ordinal {name}')
    ctx = rd(ctx_rva, ctx_size)
    regs = struct.unpack_from('<16Q', ctx, 0x78)
    rip, = struct.unpack_from('<Q', ctx, 0xf8)
    names = 'rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15'.split()
    print(' '.join(f'{n}={v:#x}' for n, v in zip(names, regs)), f'rip={rip:#x}')
    rsp = regs[4]
    print('stack:')
    shown = 0
    for i in range(0, 0x2000, 8):
        b = readmem(rsp + i, 8)
        if not b: break
        v, = struct.unpack('<Q', b)
        w = where(v)
        if w and not w.startswith(('ntdll', 'kernelbase')) or (w and shown < 4):
            print(f'  +{i:#x} {w}'); shown += 1
            if shown > 40: break
    code_bytes = readmem(rip, 16)
    if code_bytes: print('code at rip:', code_bytes.hex())

for p in sys.argv[1:]:
    print('==', p.split('\\')[-1]); main(p)
