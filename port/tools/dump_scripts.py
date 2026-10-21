"""Dump the game's menu/UI script sources from a live TTT2 guest (read-only).

TTT2's menus are scripts (C-like text calling `lib::` natives, compiled by the game's script VM
at load time). Every contiguous run of script text containing `lib::` is written to
<out>/<address>.txt, so a menu's logic can be grepped offline. Copies of the same script loaded
at several addresses are kept once (by content).

  python port/tools/dump_scripts.py --pid 123 --map port/build-o1/tekken_recomp.map --out re/scripts
"""
import argparse
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from inspect_guest import ReadProcess, parse_map  # noqa: E402

RANGES = [(0x00000000, 0x20000000), (0x40000000, 0x50000000), (0xA0000000, 0xC0000000)]
CHUNK = 32 << 20
TEXT = set(range(0x20, 0x7F)) | {0x09, 0x0A, 0x0D}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--map", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--min", type=int, default=200, help="minimum script length in bytes")
    args = ap.parse_args()
    proc = ReadProcess(args.pid)
    base = proc.resolve_guest_base(parse_map(args.map.read_text(errors="ignore")))
    args.out.mkdir(parents=True, exist_ok=True)
    seen, written = set(), 0
    for lo, hi in RANGES:
        addr = lo
        while addr < hi:
            size = min(CHUNK, hi - addr)
            try:
                data = proc.read(base + addr, size)
            except OSError:
                addr += size
                continue
            pos = 0
            while True:
                i = data.find(b"lib::", pos)
                if i < 0:
                    break
                s = i
                while s > 0 and data[s - 1] in TEXT:
                    s -= 1
                e = i
                while e < len(data) and data[e] in TEXT:
                    e += 1
                pos = e + 1
                text = data[s:e]
                if len(text) < args.min:
                    continue
                digest = hashlib.sha1(text).hexdigest()
                if digest in seen:
                    continue
                seen.add(digest)
                (args.out / f"{addr + s:08X}.txt").write_bytes(text)
                written += 1
            addr += size
    print(f"{written} distinct script texts -> {args.out}")


if __name__ == "__main__":
    main()
