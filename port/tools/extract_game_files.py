"""Extract all files from the TTT2 XGD3 disc image to re/game_files/ for the runtime VFS to serve.
Reuses the XDVDFS walk. ~7.7 GiB; run once."""
import struct, os, sys

ISO = r"D:\Projects\Tekken Tag Tournament 2 (USA, Europe)\Tekken Tag Tournament 2 (USA, Europe).iso"
BASE = 0x2080000
SECT = 2048
OUT = r"D:\Projects\Tekken Tag Tournament 2 (USA, Europe)\re\game_files"

f = open(ISO, "rb")
def read_sector(sector, count=1):
    f.seek(BASE + sector * SECT); return f.read(count * SECT)

def walk(sector, size, path, out):
    data = read_sector(sector, (size + SECT - 1)//SECT)[:size]
    def visit(w):
        off = w*4
        if off+14 > len(data): return
        left,right,start,fsize = struct.unpack_from("<HHII", data, off)
        if left == 0xFFFF: return
        attribs = data[off+12]; nlen = data[off+13]
        name = data[off+14:off+14+nlen].decode("ascii","replace")
        if left: visit(left)
        full = path + "/" + name
        if attribs & 0x10:
            if fsize: walk(start, fsize, full, out)
        else:
            out.append((full, start, fsize))
        if right: visit(right)
    visit(0)

vd = read_sector(32)
root_sector, root_size = struct.unpack_from("<II", vd, 20)
files = []
walk(root_sector, root_size, "", files)
os.makedirs(OUT, exist_ok=True)
total = sum(s for _,_,s in files); done = 0
# Skip the $SystemUpdate and AvatarAssetPack dirs (not needed by the game runtime).
for full, start, size in files:
    if full.startswith("/$SystemUpdate") or full.startswith("/AvatarAssetPack"):
        continue
    dst = os.path.join(OUT, full.lstrip("/").replace("/", os.sep))
    os.makedirs(os.path.dirname(dst) or OUT, exist_ok=True)
    f.seek(BASE + start*SECT)
    with open(dst, "wb") as g:
        remaining = size
        while remaining > 0:
            chunk = f.read(min(1<<20, remaining)); g.write(chunk); remaining -= len(chunk)
            if not chunk: break
    done += size
    print(f"  {full}  ({size}) [{done/2**30:.2f}/{total/2**30:.2f} GiB]")
f.close()
print("done ->", OUT)
