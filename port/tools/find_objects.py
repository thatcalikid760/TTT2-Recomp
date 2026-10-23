import sys, struct, numpy as np
from pathlib import Path
sys.path.insert(0, "port/tools")
from inspect_guest import ReadProcess, parse_map
pid = int(sys.argv[1]); vt = int(sys.argv[2], 16)
proc = ReadProcess(pid)
guest = proc.resolve_guest_base(parse_map(Path("port/build-o1/tekken_recomp.map").read_text(errors="ignore")))
locs = []
for lo, hi in [(0x00000000, 0x20000000), (0x40000000, 0x50000000), (0x70000000, 0x80000000), (0xA0000000, 0xC0000000)]:
    for start in range(lo, hi, 16 << 20):
        try: data = proc.read(guest + start, 16 << 20)
        except OSError: continue
        v = np.frombuffer(data, dtype=">u4")
        locs += [int(start + int(i) * 4) for i in np.nonzero(v == vt)[0]]
print("objects with vtable %08X: %d" % (vt, len(locs)))
for a in locs[:10]:
    raw = proc.read(guest + a, 0x1C0)
    u8 = lambda o: raw[o]; u32 = lambda o: struct.unpack_from(">I", raw, o)[0]
    print("obj %08X: +289=%d +400=%d +404..: %s  +424=%016X" % (a, u8(289), u32(400),
          " ".join("%08X" % u32(o) for o in range(404, 436, 4)), struct.unpack_from(">Q", raw, 424)[0]))
