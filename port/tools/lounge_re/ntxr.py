"""TTT2 NTXR textures (the Xbox 360 NUT, version 0x100): header "NTXR", u16 version, u16 count, 8 bytes, then per
texture a header (sizes after Smash Forge's NUT.cs NTP3 reader) with its pixel data right after it (version < 0x200),
an "eXt" block and a "GIDX" block holding the hash the models' materials name. Pixel data is Xbox 360 GPU memory:
16-bit words byte-swapped, and (unless measured otherwise) tiled like the GPU's textures (GpuTiledOffset2D in
port/src/runtime/kernel/gpu_render.cpp).

    python ntxr.py <archive> <offset> out_prefix     # every texture of the NTXR there, as PNGs
"""
import struct
import sys

import numpy as np

FORMATS = {0: 'DXT1', 1: 'DXT3', 2: 'DXT5', 14: 'RGBA8', 16: 'ABGR8', 17: 'RGBA8s'}


def parse(d):
    """[{hash, format, width, height, mips, data (mip 0 bytes)}] of the NTXR at d[0]."""
    assert d[:4] == b'NTXR', d[:4]
    version, count = struct.unpack_from('>HH', d, 4)
    out, at = [], 0x10
    for _ in range(count):
        total, _, dataSize, headerSize = struct.unpack_from('>IIIH', d, at)
        mips, fmt = d[at + 0x11], d[at + 0x13]
        width, height = struct.unpack_from('>HH', d, at + 0x14)
        mipSizes = list(struct.unpack_from('>%dI' % max(mips, 1), d, at + 0x30)) if mips > 1 else [dataSize]
        g = d.find(b'GIDX', at, at + headerSize)
        h = struct.unpack_from('>I', d, g + 8)[0] if g >= 0 else 0
        start = at + headerSize
        out.append(dict(hash=h, format=fmt, width=width, height=height, mips=mips,
                        data=d[start:start + mipSizes[0]]))
        at += total
    return out


def size_at(read, off):
    """Bytes of the NTXR at `off`, `read(start, end)` giving the archive's bytes (its textures' total sizes)."""
    count = struct.unpack_from('>H', read(off, off + 8), 6)[0]
    at = 0x10
    for _ in range(count):
        at += struct.unpack_from('>I', read(off + at, off + at + 4), 0)[0]
    return at


def tiled_offset(x, y, width, log_bpp):
    """GpuTiledOffset2D (gpu_render.cpp): the element index of (x, y) in a tiled surface `width` elements wide."""
    aligned = (width + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpp + 7)
    micro = ((x & 7) + ((y & 6) << 2)) << log_bpp
    offset = macro + ((micro & ~15) << 1) + (micro & 15) + ((y & 8) << (3 + log_bpp)) + ((y & 1) << 4)
    return (((offset & ~511) << 3) + ((offset & 448) << 2) + (offset & 63) +
            ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> log_bpp


def _swap16(b):
    a = np.frombuffer(b, dtype='>u2').astype('<u2')
    return a.tobytes()


def _rgb565(c):
    return np.array([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31])


def _color_block(blk, img, px, py, dxt1):
    c0, c1, bits = struct.unpack_from('<HHI', blk, 0)
    e0, e1 = _rgb565(c0), _rgb565(c1)
    if c0 > c1 or not dxt1:
        pal = [e0, e1, (2 * e0 + e1) // 3, (e0 + 2 * e1) // 3]
    else:
        pal = [e0, e1, (e0 + e1) // 2, np.zeros(3, int)]
    for k in range(16):
        img[py + k // 4, px + k % 4, :3] = pal[(bits >> (2 * k)) & 3]


def _alpha5_block(blk, img, px, py):
    a0, a1 = blk[0], blk[1]
    bits = int.from_bytes(blk[2:8], 'little')
    if a0 > a1:
        pal = [a0, a1] + [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
    else:
        pal = [a0, a1] + [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255]
    for k in range(16):
        img[py + k // 4, px + k % 4, 3] = pal[(bits >> (3 * k)) & 7]


def decode(t, tiled=True):
    """RGBA uint8 array (height, width, 4) of a texture's mip 0."""
    w, h, fmt = t['width'], t['height'], t['format']
    data = _swap16(t['data'])
    img = np.zeros((h, w, 4), np.uint8)
    img[..., 3] = 255
    if fmt in (0, 1, 2):
        bb = 8 if fmt == 0 else 16
        bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
        for by in range(bh):
            for bx in range(bw):
                i = tiled_offset(bx, by, bw, 3 if bb == 8 else 4) if tiled else by * bw + bx
                blk = data[i * bb:(i + 1) * bb]
                if len(blk) < bb:
                    continue
                if fmt == 0:
                    _color_block(blk, img, bx * 4, by * 4, True)
                else:
                    _color_block(blk[8:], img, bx * 4, by * 4, False)
                    if fmt == 2:
                        _alpha5_block(blk[:8], img, bx * 4, by * 4)
                    else:
                        a = int.from_bytes(blk[:8], 'little')
                        for k in range(16):
                            img[by * 4 + k // 4, bx * 4 + k % 4, 3] = ((a >> (4 * k)) & 15) * 17
        return img
    if fmt in (14, 16, 17):
        raw = t['data']
        for y in range(h):
            for x in range(w):
                i = tiled_offset(x, y, w, 2) if tiled else y * w + x
                px = raw[i * 4:i * 4 + 4]
                if len(px) == 4:
                    img[y, x] = [px[1], px[2], px[3], px[0]]      # ARGB in memory
        return img
    raise ValueError('texture format %d' % fmt)


if __name__ == '__main__':
    from PIL import Image
    import ttt2fs
    arc, off, prefix = int(sys.argv[1]), int(sys.argv[2], 0), sys.argv[3]
    d = ttt2fs.read(arc, off, off + size_at(lambda a, b: ttt2fs.read(arc, a, b), off))
    for k, t in enumerate(parse(d)):
        print(k, hex(t['hash']), FORMATS.get(t['format'], t['format']), t['width'], t['height'], t['mips'])
        for tiled in (True, False):
            Image.fromarray(decode(t, tiled)).save('%s_%d_%s.png' % (prefix, k, 'tiled' if tiled else 'linear'))
