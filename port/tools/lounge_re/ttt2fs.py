"""TTT2 game files: nrfo.bin names every archive's files by hash; archive i's file k spans ofsd[k]..ofsd[k+1]."""
import os
import struct

GAME = r"C:\Program Files\Tekken Tag Tournament 2\game"


def _boxes(rec):
    out, off = {}, 0
    while off + 8 <= len(rec):
        size, typ = struct.unpack_from(">I4s", rec, off)
        if size < 8:
            break
        out[typ.decode("latin1")] = rec[off + 8:off + size]
        off += size
    return out


def archives():
    """[(archive index, [(hash, start, end)])]"""
    d = open(os.path.join(GAME, "nrfo.bin"), "rb").read()
    n = struct.unpack_from(">I", d, 0)[0]
    out = []
    for i in range(n):
        off, size = struct.unpack_from(">II", d, 4 + i * 8)
        b = _boxes(d[off:off + size])
        ofsi, ofsd = b.get("ofsi", b""), b.get("ofsd", b"")
        hashes = [struct.unpack_from(">I", ofsi, k * 8)[0] for k in range(len(ofsi) // 8)]
        offs = [struct.unpack_from(">I", ofsd, k * 4)[0] for k in range(len(ofsd) // 4)]
        files = [(h, offs[k], offs[k + 1]) for k, h in enumerate(hashes) if k + 1 < len(offs)]
        out.append((i, files))
    return out


def archive_path(i):
    return os.path.join(GAME, "data%03d.bin" % i)


def read(i, start, end):
    with open(archive_path(i), "rb") as f:
        f.seek(start)
        return f.read(end - start)


def ndxr_index(path="ndxr_index.json"):
    """Every NDXR model in the archives: [(archive, offset, size, mesh count, mesh names)], cached in `path`."""
    import json
    import mmap
    if os.path.exists(path):
        return json.load(open(path))
    index = []
    for i in range(64):
        p = archive_path(i)
        if not os.path.exists(p):
            continue
        with open(p, "rb") as f:
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            at = m.find(b"NDXR")
            while at != -1:
                size = struct.unpack_from(">I", m, at + 4)[0]
                if 0x30 < size < (64 << 20) and at + size <= len(m):
                    d = m[at:at + size]
                    nsets = struct.unpack_from(">H", d, 0x0A)[0]
                    a, b, c, e = struct.unpack_from(">iiii", d, 0x10)
                    names = [x.decode("latin1") for x in d[a + 0x30 + b + c + e:size].split(b"\0") if x][:nsets]
                    index.append((i, at, size, nsets, names))
                at = m.find(b"NDXR", at + 4)
    json.dump(index, open(path, "w"))
    return index


def ntxr_index(path="ntxr_index.json"):
    """Every texture in the archives by its GIDX hash: {hash: [archive, NTXR offset, texture number]}, cached in `path`."""
    import json
    import mmap
    if os.path.exists(path):
        return {int(k): v for k, v in json.load(open(path)).items()}
    index = {}
    for i in range(64):
        p = archive_path(i)
        if not os.path.exists(p):
            continue
        with open(p, "rb") as f:
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            at = m.find(b"NTXR")
            while at != -1:
                try:
                    count = struct.unpack_from(">H", m, at + 6)[0]
                    h = at + 0x10
                    for k in range(count):
                        total, _, _, headerSize = struct.unpack_from(">IIIH", m, h)
                        g = m.find(b"GIDX", h, h + headerSize)
                        if g >= 0:
                            index.setdefault(struct.unpack_from(">I", m, g + 8)[0], [i, at, k])
                        if total <= 0:
                            break
                        h += total
                except struct.error:
                    pass
                at = m.find(b"NTXR", at + 4)
    json.dump({str(k): v for k, v in index.items()}, open(path, "w"))
    return index
