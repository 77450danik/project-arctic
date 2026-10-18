"""CUDA through nvcuda.dll (the host's driver, runtime/wine/modules/dlls/nvcuda): the driver, the card, memory there
and back. Run with the Windows Python in Arctic:  python tests/cuda/cuda_smoke.py
"""
import ctypes as C
import sys

cu = C.WinDLL("nvcuda.dll")


def ok(r, what):
    if r:
        s = C.c_char_p()
        cu.cuGetErrorString(r, C.byref(s))
        sys.exit(f"{what}: error {r} {s.value.decode() if s.value else ''}")


ok(cu.cuInit(0), "cuInit")
v = C.c_int()
ok(cu.cuDriverGetVersion(C.byref(v)), "cuDriverGetVersion")
n = C.c_int()
ok(cu.cuDeviceGetCount(C.byref(n)), "cuDeviceGetCount")
print(f"driver CUDA {v.value // 1000}.{v.value % 1000 // 10}, {n.value} device(s)")
for i in range(n.value):
    d = C.c_int()
    ok(cu.cuDeviceGet(C.byref(d), i), "cuDeviceGet")
    name = C.create_string_buffer(256)
    ok(cu.cuDeviceGetName(name, 256, d), "cuDeviceGetName")
    major, minor, mem = C.c_int(), C.c_int(), C.c_size_t()
    ok(cu.cuDeviceGetAttribute(C.byref(major), 75, d), "major")   # COMPUTE_CAPABILITY_MAJOR
    ok(cu.cuDeviceGetAttribute(C.byref(minor), 76, d), "minor")
    ok(cu.cuDeviceTotalMem_v2(C.byref(mem), d), "cuDeviceTotalMem")
    print(f"  {i}: {name.value.decode()}, sm_{major.value}{minor.value}, {mem.value >> 20} MB")
d = C.c_int()
ok(cu.cuDeviceGet(C.byref(d), 0), "cuDeviceGet")
ctx = C.c_void_p()
ok(cu.cuCtxCreate_v2(C.byref(ctx), 0, d), "cuCtxCreate")
size = 64 << 20
dptr = C.c_uint64()
ok(cu.cuMemAlloc_v2(C.byref(dptr), C.c_size_t(size)), "cuMemAlloc")
src = (C.c_ubyte * size)()
for k in range(0, size, 4096):
    src[k] = k // 4096 % 251
dst = (C.c_ubyte * size)()
ok(cu.cuMemcpyHtoD_v2(dptr, src, C.c_size_t(size)), "cuMemcpyHtoD")
ok(cu.cuMemsetD8_v2(C.c_uint64(dptr.value + size - 1), 7, C.c_size_t(1)), "cuMemsetD8")
ok(cu.cuMemcpyDtoH_v2(dst, dptr, C.c_size_t(size)), "cuMemcpyDtoH")
good = all(dst[k] == k // 4096 % 251 for k in range(0, size - 4096, 4096)) and dst[size - 1] == 7
ok(cu.cuMemFree_v2(dptr), "cuMemFree")
ok(cu.cuCtxDestroy_v2(ctx), "cuCtxDestroy")
print("64 MB to the card and back:", "ok" if good else "WRONG")
sys.exit(0 if good else 1)
