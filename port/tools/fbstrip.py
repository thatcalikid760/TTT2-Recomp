"""Contact sheet over the frame filmstrip TTT2_FBSEQ=<n> leaves in port/fb/.

A single overwritten framebuffer dump cannot tell a black screen from an animation caught
mid-fade, which is exactly the question the opening video raised. This picks the frames with
the most colour variety and tiles them in capture order.

Usage: python port/tools/fbstrip.py [dir] [out.png] [count]
"""
import glob
import os
import sys

from PIL import Image


def interest(im: Image.Image) -> float:
    """How much is going on in this frame: spread of the luminance histogram."""
    h = im.convert("L").resize((160, 90)).histogram()
    total = sum(h) or 1
    return sum(1 for c in h if c > total * 0.001)


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else "C:/ttt2/port/fb"
    out = sys.argv[2] if len(sys.argv) > 2 else "fbstrip.png"
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    paths = sorted(glob.glob(os.path.join(src, "*.bmp")))
    if not paths:
        print("no frames in", src)
        return 1
    scored = []
    for p in paths:
        im = Image.open(p).convert("RGB")
        scored.append((interest(im), p))
    scored.sort(reverse=True)
    picked = sorted(p for _, p in scored[:count])
    cols = 3
    rows = (len(picked) + cols - 1) // cols
    tw, th = 426, 240
    sheet = Image.new("RGB", (cols * tw, rows * th), (24, 24, 24))
    for i, p in enumerate(picked):
        im = Image.open(p).convert("RGB").resize((tw, th))
        sheet.paste(im, ((i % cols) * tw, (i // cols) * th))
    sheet.save(out)
    print(f"{len(paths)} frames, most interesting: "
          + ", ".join(os.path.basename(p) for p in picked))
    print("wrote", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
