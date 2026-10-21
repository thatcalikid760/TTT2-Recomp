"""Decode a texture straight out of a live TTT2 guest and save it as PNG (read-only).

Independent of both renderers: reads the guest bytes, applies the Xenos endian mode, untiles
(XGAddress2DTiledOffset, as GpuTiledOffset2D) and decodes DXT1/DXT2_3/DXT4_5 or 8_8_8_8.
Tells "the texture data is wrong" apart from "our upload/sampling is wrong".

  python port/tools/tex_dump.py --pid 123 --map port/build-o1/tekken_recomp.map \
      --base 0F205000 --fmt 18 --width 512 --height 256 --pitch 512 --tiled --endian 1 --out t.png
"""
import argparse
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from inspect_guest import ReadProcess, parse_map  # noqa: E402


def tiled_offset(x, y, width, log_bpp):
    aligned = (width + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << log_bpp
    offset = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4)
    return (((offset & ~511) << 3) + ((offset & 448) << 2) + (offset & 63) +
            ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> log_bpp


def word(data, off, endian):
    b = data[off:off + 4]
    if endian == 1:
        b = bytes([b[1], b[0], b[3], b[2]])
        return struct.unpack("<I", b)[0]
    if endian == 2:
        return struct.unpack(">I", b)[0]
    if endian == 3:
        v = struct.unpack("<I", b)[0]
        return ((v << 16) | (v >> 16)) & 0xFFFFFFFF
    return struct.unpack("<I", b)[0]


def rgb565(c):
    return np.array([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], np.uint8)


def decode_bc_color(w0, w1, four_always):
    c0, c1 = w0 & 0xFFFF, w0 >> 16
    e0, e1 = rgb565(c0).astype(int), rgb565(c1).astype(int)
    if c0 > c1 or four_always:
        pal = [e0, e1, (2 * e0 + e1) // 3, (e0 + 2 * e1) // 3]
        alpha = [255] * 4
    else:
        pal = [e0, e1, (e0 + e1) // 2, np.zeros(3, int)]
        alpha = [255, 255, 255, 0]
    out = np.zeros((4, 4, 4), np.uint8)
    for i in range(16):
        s = (w1 >> (2 * i)) & 3
        out[i // 4, i % 4, :3] = pal[s]
        out[i // 4, i % 4, 3] = alpha[s]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--map", type=Path, required=True)
    ap.add_argument("--base", required=True)
    ap.add_argument("--fmt", type=int, required=True)
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--pitch", type=int, required=True)
    ap.add_argument("--tiled", action="store_true")
    ap.add_argument("--endian", type=int, default=1)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    proc = ReadProcess(a.pid)
    guest = proc.resolve_guest_base(parse_map(a.map.read_text(errors="ignore")))
    base = int(a.base, 16)
    if base >= 0x20000000 and base < 0x80000000:
        pass
    elif base < 0x20000000:
        base |= 0xA0000000          # physical -> guest view, as PhysToGuest
    img = np.zeros((a.height, a.width, 4), np.uint8)
    if a.fmt in (18, 19, 20):
        bb = 8 if a.fmt == 18 else 16
        bw, bh = (a.pitch + 3) // 4, (a.height + 3) // 4
        aligned_rows = (bh + 31) & ~31 if a.tiled else bh
        aligned_cols = (bw + 31) & ~31 if a.tiled else bw
        data = proc.read(guest + base, aligned_cols * aligned_rows * bb)
        log_bpb = 3 if bb == 8 else 4
        for by in range((a.height + 3) // 4):
            for bx in range((a.width + 3) // 4):
                bi = tiled_offset(bx, by, bw, log_bpb) if a.tiled else by * bw + bx
                off = bi * bb
                color_off = off + (8 if bb == 16 else 0)
                blk = decode_bc_color(word(data, color_off, a.endian), word(data, color_off + 4, a.endian), bb == 16)
                if a.fmt == 20:
                    lo, hi = word(data, off, a.endian), word(data, off + 4, a.endian)
                    a0, a1 = lo & 0xFF, (lo >> 8) & 0xFF
                    idx = (hi << 16) | (lo >> 16)
                    if a0 > a1:
                        pal = [a0, a1] + [((8 - s) * a0 + (s - 1) * a1) // 7 for s in range(2, 8)]
                    else:
                        pal = [a0, a1] + [((6 - s) * a0 + (s - 1) * a1) // 5 for s in range(2, 6)] + [0, 255]
                    for i in range(16):
                        blk[i // 4, i % 4, 3] = pal[(idx >> (3 * i)) & 7]
                y0, x0 = by * 4, bx * 4
                h, w = min(4, a.height - y0), min(4, a.width - x0)
                img[y0:y0 + h, x0:x0 + w] = blk[:h, :w]
    elif a.fmt == 6:
        data = proc.read(guest + base, ((a.pitch + 31) & ~31) * ((a.height + 31) & ~31) * 4)
        for y in range(a.height):
            for x in range(a.width):
                i = tiled_offset(x, y, a.pitch, 2) if a.tiled else y * a.pitch + x
                v = word(data, i * 4, a.endian)
                img[y, x] = [(v >> 16) & 255, (v >> 8) & 255, v & 255, v >> 24]
    else:
        raise SystemExit("format %d not supported" % a.fmt)
    try:
        from PIL import Image
        Image.fromarray(img[:, :, :3]).save(a.out)
        Image.fromarray(img[:, :, 3]).save(a.out.with_name(a.out.stem + "_alpha.png"))
    except ImportError:
        np.save(a.out.with_suffix(".npy"), img)
    print("mean rgb", img[:, :, :3].reshape(-1, 3).mean(0).round(1), "mean alpha", img[:, :, 3].mean().round(1))


if __name__ == "__main__":
    main()
