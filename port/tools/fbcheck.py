"""Summarize the runtime's front-buffer dump (port/fbdump.bmp).

Headless verification for the GPU bring-up: prints how many pixels are non-black, the most
common colours, and a coarse ASCII thumbnail, then writes a PNG next to the BMP so the frame
can be looked at directly.

Usage: python port/tools/fbcheck.py [path/to/fbdump.bmp]
"""
import sys
from collections import Counter

from PIL import Image

RAMP = " .:-=+*#%@"


def main() -> int:
    path = sys.argv[1] if len(sys.argv) > 1 else "port/fbdump.bmp"
    img = Image.open(path).convert("RGB")
    w, h = img.size
    px = img.load()

    counts = Counter()
    nonblack = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            c = px[x, y]
            counts[c] += 1
            if c != (0, 0, 0):
                nonblack += 1
    sampled = sum(counts.values())
    print(f"{path}: {w}x{h}")
    print(f"sampled {sampled} px, non-black {nonblack} ({100.0 * nonblack / sampled:.2f}%)")
    print("top colours (R,G,B):")
    for c, n in counts.most_common(8):
        print(f"  {c}  {n}  ({100.0 * n / sampled:.2f}%)")

    cols, rows = 64, 24
    print("thumbnail:")
    for r in range(rows):
        line = []
        for c in range(cols):
            x = int((c + 0.5) * w / cols)
            y = int((r + 0.5) * h / rows)
            rr, gg, bb = px[x, y]
            lum = (rr * 299 + gg * 587 + bb * 114) // 1000
            line.append(RAMP[min(len(RAMP) - 1, lum * len(RAMP) // 256)])
        print("  |" + "".join(line) + "|")

    out = path.rsplit(".", 1)[0] + ".png"
    img.save(out)
    print("wrote", out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
