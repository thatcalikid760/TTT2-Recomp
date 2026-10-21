"""Extract Default.xex and write a JSON manifest of the XDVDFS disc + TU chain.
Ground-truth record for the project (re/disc_manifest.json). No decryption here."""
import struct, os, json, hashlib

ISO = r"D:\Projects\Tekken Tag Tournament 2 (USA, Europe)\Tekken Tag Tournament 2 (USA, Europe).iso"
BASE = 0x2080000
SECT = 2048
RE = r"D:\Projects\Tekken Tag Tournament 2 (USA, Europe)\re"

f = open(ISO, "rb")

def read_sector(sector, count=1):
    f.seek(BASE + sector * SECT)
    return f.read(count * SECT)

def walk_dir(sector, size, path, out):
    data = read_sector(sector, (size + SECT - 1) // SECT)[:size]
    def visit(off_words):
        off = off_words * 4
        if off + 14 > len(data):
            return
        left, right, start, fsize = struct.unpack_from("<HHII", data, off)
        if left == 0xFFFF:
            return
        attribs = data[off + 12]; nlen = data[off + 13]
        name = data[off + 14:off + 14 + nlen].decode("ascii", "replace")
        if left: visit(left)
        full = path + "/" + name
        is_dir = bool(attribs & 0x10)
        out.append(dict(path=full, sector=start, size=fsize, is_dir=is_dir))
        if is_dir and fsize:
            walk_dir(start, fsize, full, out)
        if right: visit(right)
    visit(0)

vd = read_sector(32)
root_sector, root_size = struct.unpack_from("<II", vd, 20)
entries = []
walk_dir(root_sector, root_size, "", entries)

# Extract Default.xex to re/
xex = next(e for e in entries if e["path"] == "/Default.xex")
f.seek(BASE + xex["sector"] * SECT)
xex_bytes = f.read(xex["size"])
with open(os.path.join(RE, "Default.xex"), "wb") as g:
    g.write(xex_bytes)
xex_sha = hashlib.sha1(xex_bytes).hexdigest()

# Parse XEX identity from the already-extracted file
def xex_info(d):
    opt_count = struct.unpack_from(">I", d, 0x14)[0]
    opt = {}; p = 0x18
    for _ in range(opt_count):
        k, v = struct.unpack_from(">II", d, p); p += 8; opt[k] = v
    info = {}
    if 0x00010100 in opt: info["entry_point"] = hex(opt[0x00010100])
    if 0x00010201 in opt: info["image_base"] = hex(opt[0x00010201])
    if 0x000003FF in opt:
        o = opt[0x000003FF]
        _, enc, comp = struct.unpack_from(">IHH", d, o)
        info["encryption_type"] = enc
        info["compression_type"] = comp
    if 0x00040006 in opt:
        o = opt[0x00040006]
        media, ver, bver, tid = struct.unpack_from(">IIII", d, o)
        info["media_id"] = hex(media); info["title_id"] = hex(tid)
        info["version_raw"] = hex(ver); info["base_version_raw"] = hex(bver)
    info["image_size"] = hex(struct.unpack_from(">I", d, 0xA4)[0])
    return info

manifest = {
    "iso": os.path.basename(ISO),
    "iso_size": os.path.getsize(ISO),
    "xdvdfs_partition_base": hex(BASE),
    "disc_generation": "XGD3",
    "default_xex": {
        "size": xex["size"], "sha1": xex_sha, **xex_info(xex_bytes),
    },
    "files": [
        dict(path=e["path"], size=e["size"], sector=e["sector"])
        for e in entries if not e["is_dir"]
    ],
    "title_updates": [
        {"file": "TU_174Q22P_0000008000000.0000000000082", "tu": 1, "target_version": "0.0.1.2"},
        {"file": "TU_174Q22P_0000008000000.00000000000G2", "tu": 2, "target_version": "0.0.2.2"},
        {"file": "TU_174Q22P_0000008000000.00000000000O2", "tu": 3, "target_version": "0.0.3.2",
         "latest": True, "note": "cumulative from base 0.0.0.2; supersedes TU1/TU2"},
    ],
}
with open(os.path.join(RE, "disc_manifest.json"), "w") as g:
    json.dump(manifest, g, indent=2)

print(f"Extracted Default.xex -> re/Default.xex  ({xex['size']} bytes, sha1={xex_sha})")
print(f"XEX identity: {json.dumps(xex_info(xex_bytes), indent=2)}")
print(f"Wrote re/disc_manifest.json  ({len(manifest['files'])} disc files)")
data_files = [e for e in entries if e["path"].startswith("/data") and e["path"].endswith(".bin")]
print(f"Game data archives on disc: {len(data_files)}  total={sum(e['size'] for e in data_files)/2**30:.2f} GiB")
f.close()
