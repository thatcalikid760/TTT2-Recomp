"""TTT2 NDXR (Xbox 360 NUD) reader, big-endian. Layout after Smash Forge's NUD.cs; a file may be preceded by
16-byte NU_vReg_XX entries."""
import struct
import sys

import numpy as np


def half(u):
    return float(np.frombuffer(struct.pack('<H', u), dtype=np.float16)[0])


class R:
    def __init__(self, d, at=0):
        self.d, self.at = d, at

    def u8(self):
        v = self.d[self.at]; self.at += 1; return v

    def u16(self):
        v = struct.unpack_from('>H', self.d, self.at)[0]; self.at += 2; return v

    def s16(self):
        v = struct.unpack_from('>h', self.d, self.at)[0]; self.at += 2; return v

    def u32(self):
        v = struct.unpack_from('>I', self.d, self.at)[0]; self.at += 4; return v

    def s32(self):
        v = struct.unpack_from('>i', self.d, self.at)[0]; self.at += 4; return v

    def f32(self):
        v = struct.unpack_from('>f', self.d, self.at)[0]; self.at += 4; return v

    def h16(self):
        return half(self.u16())


def strip_regs(d):
    k = 0
    while d[k:k + 4] == b'NU_v':
        k += 16
    return d[k:]


def parse(d, verbose=False):
    d = strip_regs(d) if d[:4] == b"NU_v" else d
    assert d[:4] == b'NDXR', d[:4]
    r = R(d, 4)
    size = r.u32(); version = r.u16(); nsets = r.u16(); bstart = r.s16(); bend = r.s16()
    polyStart = r.s32() + 0x30
    polySize = r.s32(); vertSize = r.s32(); vaddSize = r.s32()
    vertStart = polyStart + polySize
    vaddStart = vertStart + vertSize
    nameStart = vaddStart + vaddSize
    sphere = [r.f32() for _ in range(4)]
    if verbose:
        print('size %#x ver %#x sets %d bones %d..%d poly %#x+%#x vert %#x+%#x vadd %#x+%#x names %#x sphere %s' % (
            size, version, nsets, bstart, bend, polyStart, polySize, vertStart, vertSize, vaddStart, vaddSize, nameStart, sphere))
    objs = []
    for _ in range(nsets):
        o = {}
        o['sphere'] = [r.f32() for _ in range(4)]
        r.at += 12
        o['sortBias'] = r.f32()
        nameOff = r.s32()
        r.u16(); o['boneFlag'] = r.u16(); o['single'] = r.s16(); o['npolys'] = r.u16(); r.s32()
        end = d.index(b'\0', nameStart + nameOff)
        o['name'] = d[nameStart + nameOff:end].decode('latin1')
        objs.append(o)
    for o in objs:
        o['polys'] = []
        for _ in range(o['npolys']):
            p = {}
            p['face'] = r.s32() + polyStart
            p['vert'] = r.s32() + vertStart
            p['vadd'] = r.s32() + vaddStart
            p['nverts'] = r.u16(); p['vsize'] = r.u8(); p['uvsize'] = r.u8()
            p['tex'] = [r.s32() for _ in range(4)]
            p['nfaces'] = r.u16(); flags = r.u16(); p['strip'] = flags >> 8; p['polflag'] = flags & 0xFF
            r.at += 12
            o['polys'].append(p)
    for o in objs:
        for p in o['polys']:
            read_poly(d, o, p)
            if verbose:
                pos = p['pos']
                print('  %-24s v%5d f%5d vsize %02x uv %02x strip %02x tex %s  min %s max %s' % (
                    o['name'][:24], p['nverts'], p['nfaces'], p['vsize'], p['uvsize'], p['strip'], [hex(t) for t in p['tex']],
                    np.round(pos.min(0), 1), np.round(pos.max(0), 1)))
    return objs


def read_poly(d, o, p):
    normType = p['vsize'] & 0xF
    boneType = p['vsize'] & 0xF0
    colorType = p['uvsize'] & 0xF
    uvCount = p['uvsize'] >> 4
    n = p['nverts']
    pos = np.zeros((n, 3)); nrm = np.zeros((n, 3)); uv = np.zeros((n, 2)); bones = np.zeros((n, 4), int); wts = np.zeros((n, 4))

    def read_pos_norm(r, i):
        pos[i] = [r.f32(), r.f32(), r.f32()]
        if normType == 0:
            r.f32()
        elif normType == 1:
            r.f32(); nrm[i] = [r.f32(), r.f32(), r.f32()]; r.f32()
        elif normType in (2, 3):
            r.f32(); nrm[i] = [r.f32(), r.f32(), r.f32()]; r.f32()
            r.at += 32
        elif normType == 6:
            nrm[i] = [r.h16(), r.h16(), r.h16()]; r.h16()
        elif normType == 7:
            nrm[i] = [r.h16(), r.h16(), r.h16()]; r.h16(); r.at += 16
        else:
            raise ValueError('normal type %d' % normType)

    def read_bones(r, i):
        if boneType == 0:
            bones[i] = [o['single'], 0, 0, 0]; wts[i] = [1, 0, 0, 0]
        elif boneType == 0x10:
            bones[i] = [r.u32() for _ in range(4)]; wts[i] = [r.f32() for _ in range(4)]
        elif boneType == 0x20:
            bones[i] = [r.u16() for _ in range(4)]; wts[i] = [r.h16() for _ in range(4)]
        elif boneType == 0x40:
            bones[i] = [r.u8() for _ in range(4)]; wts[i] = [r.u8() / 255 for _ in range(4)]
        else:
            raise ValueError('bone type %#x' % boneType)

    def read_uv(r, i):
        if colorType == 2:
            r.at += 4
        elif colorType == 4:
            r.at += 8
        elif colorType != 0:
            raise ValueError('color type %d' % colorType)
        for k in range(uvCount):
            u, v = r.h16(), r.h16()
            if k == 0:
                uv[i] = [u, v]

    if boneType > 0:
        r = R(d, p['vert'])
        for i in range(n):
            read_uv(r, i)
        r = R(d, p['vadd'])
        for i in range(n):
            read_pos_norm(r, i); read_bones(r, i)
    else:
        r = R(d, p['vert'])
        for i in range(n):
            read_pos_norm(r, i); read_bones(r, i); read_uv(r, i)
    r = R(d, p['face'])
    idx = [r.u16() for _ in range(p['nfaces'])]
    tris = []
    if p['strip'] in (0, 0x40):                 # triangle strip, 0xFFFF restarts (TTT2: flag 0)
        run = []
        for v in idx + [0xFFFF]:
            if v == 0xFFFF:
                for k in range(len(run) - 2):
                    a, b, c = run[k], run[k + 1], run[k + 2]
                    if a != b and b != c and a != c:
                        tris.append((a, b, c) if k % 2 == 0 else (b, a, c))
                run = []
            else:
                run.append(v)
    else:
        tris = [tuple(idx[k:k + 3]) for k in range(0, len(idx) - 2, 3)]
    p.update(pos=pos, nrm=nrm, uv=uv, bones=bones, wts=wts, tris=np.array(tris, int).reshape(-1, 3))


if __name__ == '__main__':
    parse(open(sys.argv[1], 'rb').read(), verbose=True)
