import struct, re, os
def boxes(rec):
    out, off = {}, 0
    while off + 8 <= len(rec):
        size, typ = struct.unpack_from(">I4s", rec, off)
        if size < 8: break
        out.setdefault(typ.decode("latin1"), []).append((off, size)); off += size
    return out
def hashes(path):
    data = open(path, "rb").read()
    count = struct.unpack_from(">I", data, 0)[0]
    known = {}
    for i in range(count):
        off, size = struct.unpack_from(">II", data, 4 + i * 8)
        rec = data[off:off + size]
        b = boxes(rec)
        if "ofsi" not in b: continue
        o, s = b["ofsi"][0]
        n = (s - 8) // 8
        for k in range(n):
            h, tag = struct.unpack_from(">II", rec, o + 8 + k * 8)
            known.setdefault(h, (i, k))
    return count, known
roots = ["re/game_files_x"]
for cand in ["re/game_files_x/nrfo.bin"]:
    pass
gc, gk = hashes("re/game_files_x/nrfo.bin")
print("game nrfo: %d records, %d hashes" % (gc, len(gk)))
upd = None
for p in ["re/game_files_x/update/nrfo.bin", "re/game_files_x/Update/nrfo.bin", "re/update/nrfo.bin"]:
    if os.path.exists(p): upd = p
print("update nrfo:", upd)
uk = {}
if upd:
    uc, uk = hashes(upd); print("update nrfo: %d records, %d hashes" % (uc, len(uk)))
log = open("re/run_menu7_20261010.txt", errors="ignore").read()
probed = list(dict.fromkeys(re.findall(r"hash=([0-9A-F]{8})", log)))
missing = [h for h in probed if int(h, 16) not in gk and int(h, 16) not in uk]
print("probed %d, missing from both tables: %d" % (len(probed), len(missing)))
print(" ".join(missing[:60]))
