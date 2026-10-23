"""Find text in a live TTT2 guest address space (read-only): ASCII, UTF-16BE and UTF-16LE.

For UI work: locate the string tables / message files behind a menu row's label, then the
code that references them.

  python port/tools/guest_strings.py --pid 123 --map port/build-o1/tekken_recomp.map \
      --text "3D DEPTH" --text "MOTION BLUR" [--context 64]
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from inspect_guest import ReadProcess, parse_map  # noqa: E402

RANGES = [(0x00000000, 0x20000000), (0x40000000, 0x50000000), (0x70000000, 0x80000000),
          (0x82000000, 0x84000000), (0xA0000000, 0xC0000000)]
CHUNK = 16 << 20


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--map", type=Path, required=True)
    ap.add_argument("--text", action="append", required=True)
    ap.add_argument("--context", type=int, default=48)
    ap.add_argument("--max", type=int, default=30)
    args = ap.parse_args()
    proc = ReadProcess(args.pid)
    base = proc.resolve_guest_base(parse_map(args.map.read_text(errors="ignore")))
    needles = []
    for t in args.text:
        needles.append((t, "ascii", t.encode("ascii")))
        needles.append((t, "utf16be", t.encode("utf-16-be")))
        needles.append((t, "utf16le", t.encode("utf-16-le")))
    hits = {n: 0 for n in needles}
    for lo, hi in RANGES:
        addr = lo
        while addr < hi:
            size = min(CHUNK, hi - addr)
            try:
                data = proc.read(base + addr, size + 64)
            except OSError:
                addr += size
                continue
            for n in needles:
                text, kind, pat = n
                start = 0
                while hits[n] < args.max:
                    i = data.find(pat, start)
                    if i < 0 or i >= size:
                        break
                    hits[n] += 1
                    a = addr + i
                    ctx = data[max(0, i - args.context // 2): i + len(pat) + args.context // 2]
                    shown = ctx.decode("utf-16-be" if kind == "utf16be" else "utf-16-le" if kind == "utf16le" else "latin-1",
                                       errors="replace").replace("\x00", ".")
                    print(f"{a:08X} {kind:7s} {text!r}: {ascii(shown)}")
                    start = i + 1
            addr += size
    for (text, kind, _), n in hits.items():
        if n:
            print(f"-- {text!r} {kind}: {n} hit(s)")


if __name__ == "__main__":
    main()
