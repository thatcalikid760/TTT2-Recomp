#!/usr/bin/env python3
"""Measure a native-size frame dump (TTT2_FB_NATIVE=1 -> C:/ttt2/port/fb_native.bmp).

Reports the image size and a sharpness figure (mean absolute Laplacian of luma, computed on the
native image and on the same image box-downscaled to 1280x720 then upscaled back): a true
higher render resolution keeps detail that the 720p round trip loses, so ratio > 1 means the
extra pixels carry real detail (1.0 = an upscaled 720p image).
Optionally writes PNG crops for inspection: --crop x0,y0,x1,y1 in 1280x720 units (scaled).

Usage: render_scale_check.py [bmp] [--out dir] [--crop x0,y0,x1,y1 ...]
"""
import argparse
import os

import numpy as np
from PIL import Image


def laplacian_energy(luma: np.ndarray) -> float:
    c = luma[1:-1, 1:-1]
    lap = 4 * c - luma[:-2, 1:-1] - luma[2:, 1:-1] - luma[1:-1, :-2] - luma[1:-1, 2:]
    return float(np.mean(np.abs(lap)))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("bmp", nargs="?", default="C:/ttt2/port/fb_native.bmp")
    ap.add_argument("--out", default=None)
    ap.add_argument("--crop", action="append", default=[])
    a = ap.parse_args()
    img = Image.open(a.bmp).convert("RGB")
    w, h = img.size
    scale = w / 1280.0
    luma = np.asarray(img.convert("L"), dtype=np.float32)
    native = laplacian_energy(luma)
    round_trip = img.resize((1280, 720), Image.BOX).resize((w, h), Image.BILINEAR)
    rt = laplacian_energy(np.asarray(round_trip.convert("L"), dtype=np.float32))
    mean = np.asarray(img, dtype=np.float32).reshape(-1, 3).mean(axis=0)
    print(f"{a.bmp}: {w}x{h} (scale {scale:.2f}) mean rgb {mean.round(1).tolist()} "
          f"sharpness native {native:.3f} / 720p round trip {rt:.3f} = ratio {native / max(rt, 1e-6):.2f}")
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        for i, c in enumerate(a.crop):
            x0, y0, x1, y1 = (float(v) for v in c.split(","))
            box = tuple(int(round(v * scale)) for v in (x0, y0, x1, y1))
            path = os.path.join(a.out, f"crop{i}_{w}x{h}.png")
            img.crop(box).save(path)
            print("wrote", path, box)


if __name__ == "__main__":
    main()
