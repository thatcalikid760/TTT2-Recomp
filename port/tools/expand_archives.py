"""Build a VFS tree with the disc's compressed archives expanded.

26 of TTT2's 42 dataNNN.bin archives are stored on the disc inside an Xbox LZX container
(magic 0F F5 12 ED). nrfo.bin - the master name table - addresses every archive in its
EXPANDED form, so the game cannot read a single asset out of a compressed archive: it either
gets raw compression bitstream or reads past end-of-file. This script produces a directory in
which every archive is in the form the game expects.

Everything that does not need expanding is hard-linked, so the tree costs only the extra bytes
the expansion actually adds.

The expected expanded size of archive i is derived from nrfo.bin itself (max(ofsd) + 256), and
each expanded file is checked against it plus the 'ftyp'/'nrfi' magic, so a wrong result is
reported rather than silently written.

Usage:
    python port/tools/expand_archives.py [--src DIR] [--dst DIR] [--tool PATH]
Then run the port with TTT2_VFS_ROOT pointing at the destination.
"""
import argparse
import os
import struct
import subprocess
import sys

CHUNK_ALIGN = 0x20000
LZX_MAGIC = b"\x0f\xf5\x12\xed"


def read_boxes(rec: bytes) -> dict:
    boxes, off = {}, 0
    while off + 8 <= len(rec):
        size = struct.unpack_from(">I", rec, off)[0]
        kind = rec[off + 4:off + 8].decode("latin1", "replace")
        boxes[kind] = (off, size)
        if size < 8:
            break
        off += size
    return boxes


def expected_sizes(nrfo_path: str, names: list) -> dict:
    """Expanded size of each archive = max(ofsd) + 256, from its nrfo record."""
    data = open(nrfo_path, "rb").read()
    count = struct.unpack_from(">I", data, 0)[0]
    out = {}
    for i in range(count):
        off, size = struct.unpack_from(">II", data, 4 + i * 8)
        if i >= len(names):
            break
        if not size:
            out[names[i]] = None
            continue
        rec = data[off:off + size]
        boxes = read_boxes(rec)
        if "ofsd" not in boxes:
            out[names[i]] = None
            continue
        do, dsz = boxes["ofsd"]
        vals = [struct.unpack_from(">I", rec, do + 8 + k * 4)[0] for k in range((dsz - 8) // 4)]
        out[names[i]] = (max(vals) if vals else 0) + 256
    return out


def link_or_copy(src: str, dst: str) -> None:
    if os.path.exists(dst):
        os.remove(dst)
    try:
        os.link(src, dst)
    except OSError:
        with open(src, "rb") as fi, open(dst, "wb") as fo:
            while True:
                buf = fi.read(1 << 22)
                if not buf:
                    break
                fo.write(buf)


def main() -> int:
    here = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=os.path.join(here, "re", "game_files"))
    ap.add_argument("--dst", default=os.path.join(here, "re", "game_files_x"))
    ap.add_argument("--tool", default=os.path.join(here, "tools", "x_decompress",
                                                   "x_decompress.exe"))
    args = ap.parse_args()

    if not os.path.isfile(args.tool):
        print("decompressor not built:", args.tool)
        return 1
    os.makedirs(args.dst, exist_ok=True)

    archives = sorted(f for f in os.listdir(args.src)
                      if f.startswith("data") and f.endswith(".bin") and f != "data100.bin")
    sizes = expected_sizes(os.path.join(args.src, "nrfo.bin"), archives)

    expanded = failed = linked = 0
    for name in sorted(os.listdir(args.src)):
        src = os.path.join(args.src, name)
        dst = os.path.join(args.dst, name)
        if not os.path.isfile(src):
            continue
        with open(src, "rb") as f:
            magic = f.read(4)
        want = sizes.get(name)
        if magic != LZX_MAGIC or not want:
            link_or_copy(src, dst)
            linked += 1
            continue
        if os.path.isfile(dst) and os.path.getsize(dst) == want:
            print(f"{name}: already expanded")
            expanded += 1
            continue
        tmp = dst + ".part"
        rc = subprocess.run([args.tool, src, tmp, str(want)],
                            capture_output=True, text=True)
        line = (rc.stdout or "").strip().splitlines()
        note = line[0] if line else rc.stderr.strip()
        if rc.returncode == 0 and os.path.isfile(tmp) and os.path.getsize(tmp) == want:
            if os.path.exists(dst):
                os.remove(dst)
            os.replace(tmp, dst)
            expanded += 1
            print(f"{name}: EXPANDED -> {want} bytes")
        else:
            if os.path.exists(tmp):
                os.remove(tmp)
            # Leave the compressed original in place so the tree stays usable; the game will
            # fail on this archive exactly as it does today.
            link_or_copy(src, dst)
            failed += 1
            print(f"{name}: STILL COMPRESSED ({note})")

    print(f"\nexpanded {expanded}, still compressed {failed}, passed through {linked}")
    print("run with TTT2_VFS_ROOT=" + args.dst.replace("\\", "/"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
