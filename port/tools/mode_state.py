import sys, struct
from pathlib import Path
sys.path.insert(0, "port/tools")
from inspect_guest import ReadProcess, parse_map
proc = ReadProcess(int(sys.argv[1]))
g = proc.resolve_guest_base(parse_map(Path("port/build-o1/tekken_recomp.map").read_text(errors="ignore")))
def readg(a, n): return proc.read(g + a, n)
def u32(a): return struct.unpack(">I", readg(a, 4))[0]
mode = u32(0x82F30B94)
vt, obj, req, cur = struct.unpack(">4I", readg(mode, 16))
print("mode impl %08X obj %08X requested %d current %d" % (mode, obj, req, cur))
ovt, impl = struct.unpack(">2I", readg(obj, 8))
print("object vtable %08X impl %08X" % (ovt, impl))
hdr = readg(impl, 140)
for off in range(0, 140, 4):
    v = struct.unpack_from(">I", hdr, off)[0]
    note = ""
    if 0x82000000 <= v < 0x82100000:
        s = readg(v, 80).split(b"\0")[0]
        if s and all(32 <= c < 127 for c in s): note = "  '" + s.decode() + "'"
    print("  +%3d: %08X%s" % (off, v, note))
print("state clock", struct.unpack_from(">Q", hdr, 40)[0])
