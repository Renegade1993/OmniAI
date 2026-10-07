"""dmp_exc.py DUMP: the exception record of a Windows minidump (stream type 6): thread, code, address. October 7th (lane 10's
crash in RebalanceStacks, the same log as K's crash of October 6th). Read-only."""
import struct
import sys

CODES = {0xC0000094: "EXCEPTION_INT_DIVIDE_BY_ZERO", 0xC0000005: "EXCEPTION_ACCESS_VIOLATION", 0xE06D7363: "C++ exception (MSVC throw)",
         0xC0000409: "STACK_BUFFER_OVERRUN / fail fast", 0x80000003: "BREAKPOINT", 0xC00000FD: "STACK_OVERFLOW"}
d = open(sys.argv[1], "rb").read()
sig, ver, nstreams, diroff = struct.unpack_from("<4sIII", d, 0)
assert sig == b"MDMP", sig
mods = []
for i in range(nstreams):
    stype, size, rva = struct.unpack_from("<III", d, diroff + 12 * i)
    if stype == 6:
        tid, _align = struct.unpack_from("<II", d, rva)
        code, flags, rec, addr, nparams = struct.unpack_from("<IIQQI", d, rva + 8)
        print("thread %d exception 0x%08X %s at 0x%X" % (tid, code, CODES.get(code, "?"), addr))
    if stype == 4:  # module list: find the module holding the address
        n = struct.unpack_from("<I", d, rva)[0]
        for k in range(n):
            base, msize = struct.unpack_from("<QI", d, rva + 4 + 108 * k)
            name_rva = struct.unpack_from("<I", d, rva + 4 + 108 * k + 20)[0]
            ln = struct.unpack_from("<I", d, name_rva)[0]
            name = d[name_rva + 4:name_rva + 4 + ln].decode("utf-16-le", "replace")
            mods.append((base, msize, name))
for i in range(nstreams):
    stype, size, rva = struct.unpack_from("<III", d, diroff + 12 * i)
    if stype == 6:
        addr = struct.unpack_from("<Q", d, rva + 8 + 16)[0]
        for base, msize, name in mods:
            if base <= addr < base + msize:
                print("  in %s +0x%X" % (name.split("\\")[-1], addr - base))
