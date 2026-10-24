"""Flat-shaded software render of NDXR models (bind pose) to a PNG: render.py out.png name1 name2 ... [--yaw deg]"""
import json
import math
import sys

import numpy as np
from PIL import Image

import ndxr
import ntxr
import ttt2fs

_TEX = {}


def texture(h):
    """RGBA array of the texture with GIDX hash `h` (None when not found)."""
    if h not in _TEX:
        index = ttt2fs.ntxr_index()
        if h not in index:
            _TEX[h] = None
        else:
            i, at, k = index[h]
            d = ttt2fs.read(i, at, at + ntxr.size_at(lambda a, b: ttt2fs.read(i, a, b), at))
            _TEX[h] = ntxr.decode(ntxr.parse(d)[k], tiled=False)
    return _TEX[h]


def load(name):
    for i, at, size, n, names in ttt2fs.ndxr_index():
        if name in names:
            return ndxr.parse(ttt2fs.read(i, at, at + size))
    raise KeyError(name)


def render(models, out, yaw=20, W=600, H=900):
    tris, cols, uvs, texs = [], [], [], []
    palette = [(200, 170, 150), (90, 120, 200), (200, 90, 90), (90, 180, 110), (190, 180, 80), (150, 100, 190)]
    k = 0
    for objs in models:
        for o in objs:
            for p in o['polys']:
                t = p['pos'][p['tris']]
                tris.append(t)
                uvs.append(p['uv'][p['tris']])
                tex = texture(p['textures'][0]) if p['textures'] else None
                texs += [tex] * len(t)
                cols += [palette[k % len(palette)]] * len(t)
                k += 1
    T = np.concatenate(tris)
    UV = np.concatenate(uvs)
    a = math.radians(yaw)
    rot = np.array([[math.cos(a), 0, math.sin(a)], [0, 1, 0], [-math.sin(a), 0, math.cos(a)]])
    T = T @ rot.T
    lo, hi = T.reshape(-1, 3).min(0), T.reshape(-1, 3).max(0)
    s = 0.9 * min(W / (hi[0] - lo[0] + 1e-6), H / (hi[1] - lo[1] + 1e-6))
    cx, cy = (lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2
    img = np.full((H, W, 3), 30, np.uint8)
    z = np.full((H, W), -1e30)
    light = np.array([0.3, 0.5, 0.8]); light /= np.linalg.norm(light)
    for t, c, uv, tex in zip(T, cols, UV, texs):
        n = np.cross(t[1] - t[0], t[2] - t[0]); ln = np.linalg.norm(n)
        if ln == 0:
            continue
        shade = 0.25 + 0.75 * abs(n @ light) / ln
        xs = (t[:, 0] - cx) * s + W / 2; ys = H / 2 - (t[:, 1] - cy) * s
        x0, x1 = int(max(0, xs.min())), int(min(W - 1, xs.max())); y0, y1 = int(max(0, ys.min())), int(min(H - 1, ys.max()))
        if x1 < x0 or y1 < y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        d = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0])
        if abs(d) < 1e-9:
            continue
        w1 = ((gx - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (gy - ys[0])) / d
        w2 = ((xs[1] - xs[0]) * (gy - ys[0]) - (gx - xs[0]) * (ys[1] - ys[0])) / d
        w0 = 1 - w1 - w2
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        zz = w0 * t[0, 2] + w1 * t[1, 2] + w2 * t[2, 2]
        sub = z[y0:y1 + 1, x0:x1 + 1]
        m = inside & (zz > sub)
        sub[m] = zz[m]
        if tex is not None:
            u = (w0 * uv[0, 0] + w1 * uv[1, 0] + w2 * uv[2, 0])[m]
            v = (w0 * uv[0, 1] + w1 * uv[1, 1] + w2 * uv[2, 1])[m]
            th, tw = tex.shape[:2]
            tx = (np.floor(u * tw).astype(int)) % tw
            ty = (np.floor(v * th).astype(int)) % th
            rgb = tex[ty, tx, :3].astype(float) * (0.45 + 0.55 * shade)
            img[y0:y1 + 1, x0:x1 + 1][m] = np.clip(rgb, 0, 255).astype(np.uint8)
        else:
            img[y0:y1 + 1, x0:x1 + 1][m] = (np.array(c) * shade).astype(np.uint8)
    Image.fromarray(img).save(out)
    print(out, 'tris', len(T), 'bounds', np.round(lo), np.round(hi))


if __name__ == '__main__':
    args = sys.argv[1:]
    yaw = 20
    if '--yaw' in args:
        k = args.index('--yaw'); yaw = float(args[k + 1]); del args[k:k + 2]
    render([load(n) for n in args[1:]], args[0], yaw)
