import ctypes
import sys

# Checks that CUDA compute works on every device: at least one device, each device's PCI address and UUID, a context,
# and a 1 MiB device memory round trip (allocate, fill, copy back, compare, free). Needs only the Python standard
# library and the driver's libcuda.so.1; exits non-zero on the first failure
SIZE = 1 << 20


def check(result: int, call: str) -> None:
    if result != 0:
        sys.exit(f"{call} failed: CUresult {result}")


if __name__ == "__main__":
    cu = ctypes.CDLL("libcuda.so.1")
    cu.cuMemAlloc_v2.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.c_size_t]
    cu.cuMemsetD8_v2.argtypes = [ctypes.c_uint64, ctypes.c_ubyte, ctypes.c_size_t]
    cu.cuMemcpyDtoH_v2.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_size_t]
    cu.cuMemFree_v2.argtypes = [ctypes.c_uint64]
    check(cu.cuInit(0), "cuInit")
    count = ctypes.c_int()
    check(cu.cuDeviceGetCount(ctypes.byref(count)), "cuDeviceGetCount")
    if count.value == 0:
        sys.exit("no CUDA devices")
    for ordinal in range(count.value):
        device, bus, uuid = ctypes.c_int(), ctypes.create_string_buffer(64), (ctypes.c_ubyte * 16)()
        check(cu.cuDeviceGet(ctypes.byref(device), ordinal), "cuDeviceGet")
        check(cu.cuDeviceGetPCIBusId(bus, 64, device), "cuDeviceGetPCIBusId")
        check(cu.cuDeviceGetUuid(uuid, device), "cuDeviceGetUuid")
        ctx, ptr, host = ctypes.c_void_p(), ctypes.c_uint64(), ctypes.create_string_buffer(SIZE)
        check(cu.cuCtxCreate_v2(ctypes.byref(ctx), 0, device), "cuCtxCreate")
        check(cu.cuMemAlloc_v2(ctypes.byref(ptr), SIZE), "cuMemAlloc")
        check(cu.cuMemsetD8_v2(ptr, 0x5A, SIZE), "cuMemsetD8")
        check(cu.cuMemcpyDtoH_v2(host, ptr, SIZE), "cuMemcpyDtoH")
        if host.raw != b"\x5a" * SIZE:
            sys.exit(f"cuda device {ordinal}: device memory read back wrong")
        check(cu.cuMemFree_v2(ptr), "cuMemFree")
        check(cu.cuCtxDestroy_v2(ctx), "cuCtxDestroy")
        print(f"cuda device {ordinal}: {bus.value.decode()} uuid {bytes(uuid).hex()} context and memory ok")
    print(f"cuda ok, {count.value} devices")
