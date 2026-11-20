#!/usr/bin/env python3
"""WebGPU in Arctic's Chrome on Vulkan, as Chrome does on Linux.

Chrome for Windows hands WebGPU to Dawn's Direct3D 12 backend only (Vulkan is
there just for SwiftShader). In Arctic Direct3D 12 is vkd3d-proton over Vulkan,
so a WebGPU page went WGSL -> HLSL -> DXC -> DXIL -> vkd3d -> SPIR-V -> driver;
DXC alone took 38 % of the CPU in a WebGPU game, and full screen fell to
5 fps. Dawn's Vulkan backend is in chrome.dll: three branches of the GPU
process choose it and let it show the canvas (docs/M3-gpu.md, "WebGPU у
Chrome через Vulkan"):

1. WebGPUDecoderImpl::CreatePreferredAdapter builds the backend list of the
   default adapter from a const array {D3D11, OpenGLES, Vulkan, D3D12}: the
   pair of lea that takes [D3D12] is moved 4 bytes back, to [Vulkan].
2. Its CanUseAdapter accepts an adapter only with external images or when it
   is SwiftShader; Dawn's Vulkan backend has no external memory on Windows:
   the jne after SupportsExternalImages() becomes a jmp.
3. HandleAssociateMailboxImmediate gives a CPU adapter's canvas to the Skia
   path (copies) and any other to the D3D shared image, which takes only
   D3D devices: the je after "adapterType == CPU" becomes a jmp.

The places are found by their bytes, each exactly once; when one is not (a
Chrome whose code changed), nothing is written and Chrome stays on D3D12.
Usage: patch-chrome-webgpu.py chrome.dll [--check]
"""
import re
import struct
import sys


def sections(data):
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    optsize = struct.unpack_from('<H', data, pe + 20)[0]
    base = struct.unpack_from('<Q', data, pe + 24 + 24)[0]
    out = {}
    off = pe + 24 + optsize
    for _ in range(nsec):
        name = data[off:off + 8].rstrip(b'\0').decode(errors='replace')
        vsize, va, rawsize, raw = struct.unpack_from('<IIII', data, off + 8)
        out[name] = (va, rawsize, raw)
        off += 40
    return base, out


def find_backend_list(data, text, rdata):
    t_va, t_size, t_raw = text
    r_va, r_size, r_raw = rdata
    arrays = [m.start() for m in re.finditer(re.escape(struct.pack('<4i', 3, 8, 6, 4)), data[r_raw:r_raw + r_size])]
    hits = []
    pattern = re.compile(rb'\x48\x8d\x15(....)\x4c\x8d\x05(....)', re.S)
    for a in arrays:
        arr_va = r_va + a
        for m in pattern.finditer(data, t_raw, t_raw + t_size):
            ip1 = m.start() - t_raw + t_va + 7
            ip2 = ip1 + 7
            d1 = struct.unpack('<i', m.group(1))[0]
            d2 = struct.unpack('<i', m.group(2))[0]
            if ip1 + d1 == arr_va + 12 and ip2 + d2 == arr_va + 16:
                hits.append(m.start())
    return hits


def find_external_images(data, text):
    t_va, t_size, t_raw = text
    hits = []
    for m in re.finditer(rb'\x84\xc0\x0f\x85....\x40\x84\xff\x0f\x85', data[t_raw:t_raw + t_size], re.S):
        p = t_raw + m.start()
        before = data[p - 96:p]
        if b'\xe0\x1a\x00\x00' in before and b'\xde\xc0\x00\x00' in before:   # vendor 0x1AE0, device 0xC0DE: SwiftShader
            hits.append(p + 2)
    return hits


def find_cpu_canvas(data, text, near):
    """the pattern is not unique in chrome.dll: the one of the WebGPU decoder
    lies in the same object file as CanUseAdapter, a few KB after it"""
    t_va, t_size, t_raw = text
    hits = [t_raw + m.start() + 13 for m in
            re.finditer(rb'\x48\x85\xc0\x0f\x84....\x83\x78.\x03\x74', data[t_raw:t_raw + t_size], re.S)]
    return [h for h in hits if near is not None and 0 < h - near < 0x10000]


def main():
    path = sys.argv[1]
    check = '--check' in sys.argv[2:]
    data = bytearray(open(path, 'rb').read())
    base, secs = sections(data)
    text, rdata = secs.get('.text'), secs.get('.rdata')
    if not text or not rdata:
        print(f'{path}: no .text/.rdata, left as it is')
        return 1

    lists = find_backend_list(data, text, rdata)
    external = find_external_images(data, text)
    canvas = find_cpu_canvas(data, text, external[0] if len(external) == 1 else None)
    print(f'backend list: {len(lists)}, external images: {len(external)}, cpu canvas: {len(canvas)}')
    if (len(lists), len(external), len(canvas)) == (0, 0, 0):
        print(f'{path}: none of the places: patched already, or a Chrome whose code changed; left as it is')
        return 1
    if (len(lists), len(external), len(canvas)) != (1, 1, 1):
        print(f'{path}: not exactly one of each place, left as it is (WebGPU stays on Direct3D 12)')
        return 1
    if check:
        return 0

    p = lists[0]
    for o in (p + 3, p + 10):
        d = struct.unpack_from('<i', data, o)[0]
        struct.pack_into('<i', data, o, d - 4)
    p = external[0]
    assert data[p:p + 2] == b'\x0f\x85'
    data[p:p + 2] = b'\x90\xe9'
    p = canvas[0]
    assert data[p] == 0x74
    data[p] = 0xEB
    open(path, 'wb').write(data)
    print(f'{path}: WebGPU on Vulkan')
    return 0


if __name__ == '__main__':
    sys.exit(main())
