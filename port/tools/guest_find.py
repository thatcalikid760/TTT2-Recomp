"""Search a live TTT2 guest address space for big-endian floats (read-only).

Finds every 4-byte-aligned location holding a value within --tol of each target and reports
locations where all targets sit within --window bytes of the first one (e.g. matrix rows).

  python port/tools/guest_find.py --pid 123 --map port/build-o1/tekken_recomp.map \
      --float 2.14457 --float 3.65463 --window 64
"""
import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from inspect_guest import ReadProcess, parse_map  # noqa: E402

# Guest ranges worth scanning: the physical/virtual heaps and the stacks/TLS area.
RANGES = [(0x00000000, 0x20000000), (0x40000000, 0x50000000), (0x70000000, 0x80000000),
          (0xA0000000, 0xC0000000)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--map", type=Path, required=True)
    ap.add_argument("--float", type=float, action="append", required=True, dest="floats")
    ap.add_argument("--tol", type=float, default=2e-4)
    ap.add_argument("--window", type=int, default=64)
    ap.add_argument("--max", type=int, default=40)
    args = ap.parse_args()

    proc = ReadProcess(args.pid)
    guest = proc.resolve_guest_base(parse_map(args.map.read_text(errors="ignore")))
    hits = [[] for _ in args.floats]
    chunk = 16 << 20
    for lo, hi in RANGES:
        for start in range(lo, hi, chunk):
            try:
                data = proc.read(guest + start, chunk)
            except OSError:
                continue
            vals = np.frombuffer(data, dtype=">f4")
            for t, target in enumerate(args.floats):
                idx = np.nonzero(np.abs(vals - np.float32(target)) <= args.tol)[0]
                hits[t].extend((start + idx * 4).tolist())
    for t, target in enumerate(args.floats):
        print("target %g: %d hits" % (target, len(hits[t])))
    others = [np.array(sorted(h)) for h in hits[1:]]
    shown = 0
    for a in sorted(hits[0]):
        if all(o.size and np.min(np.abs(o - a)) <= args.window for o in others):
            print("cluster at guest %08X" % a)
            shown += 1
            if shown >= args.max:
                break


if __name__ == "__main__":
    main()
