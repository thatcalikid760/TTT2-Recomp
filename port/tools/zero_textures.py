import sys, re, numpy as np
from pathlib import Path
sys.path.insert(0, "port/tools")
from inspect_guest import ReadProcess, parse_map
proc = ReadProcess(int(sys.argv[1]))
g = proc.resolve_guest_base(parse_map(Path("port/build-o1/tekken_recomp.map").read_text(errors="ignore")))
seen = {}
for line in open(sys.argv[2], errors="ignore"):
    m = re.match(r"\s+t0: base=(\w+) fmt=18 tiled endian=\d (\d+)x(\d+)", line)
    if m:
        seen[m.group(1)] = (int(m.group(2)), int(m.group(3)))
for b, (w, h) in sorted(seen.items()):
    n = w * h // 2
    data = np.frombuffer(proc.read(g + (int(b, 16) | 0xA0000000), n), np.uint8)
    print("%s %dx%d %s" % (b, w, h, "ZERO" if not data.any() else "data"))
