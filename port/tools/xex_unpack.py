"""Decrypt + basic-decompress an Xbox 360 XEX2 basefile (retail key) and locate the
register save/restore stubs by XenonRecomp's documented byte patterns.

Purpose: independently validate the XenonRecomp TOML save/restore addresses against
OUR image (issue #34's values were for an unknown build). Read-only inspection; the
output image is written to re/decrypted/ (gitignored) for local analysis only.

BASIC compression + normal encryption only (matches TTT2: enc=1, comp=1).
"""
import struct, os, sys
from Crypto.Cipher import AES

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")

# XenonRecomp README byte patterns (start of each stub), + expected VA from issue #34
PATTERNS = {
    "restgprlr_14": (bytes.fromhex("e9c1ff68"), 0x82bafea0),
    "savegprlr_14": (bytes.fromhex("f9c1ff68"), 0x82bafe50),
    "restfpr_14":   (bytes.fromhex("c9ccff70"), 0x82bb0b3c),
    "savefpr_14":   (bytes.fromhex("d9ccff70"), 0x82bb0af0),
    "restvmx_14":   (bytes.fromhex("3960fee07dcb60ce"), 0x82bb1d58),
    "savevmx_14":   (bytes.fromhex("3960fee07dcb61ce"), 0x82bb1ac0),
    "restvmx_64":   (bytes.fromhex("3960fc00100b60cb"), 0x82bb1dec),
    "savevmx_64":   (bytes.fromhex("3960fc00100b61cb"), 0x82bb1b54),
}

def unpack(path):
    d = open(path, "rb").read()
    assert d[:4] == b"XEX2", d[:4]
    header_size = struct.unpack_from(">I", d, 0x08)[0]      # offset to basefile data
    sec_off     = struct.unpack_from(">I", d, 0x10)[0]
    opt_count   = struct.unpack_from(">I", d, 0x14)[0]
    opt = {}; p = 0x18
    for _ in range(opt_count):
        k, v = struct.unpack_from(">II", d, p); p += 8; opt[k] = v

    # FILE_FORMAT_INFO (0x3FF) — may be absent on a patcher-produced XEX
    load_address = struct.unpack_from(">I", d, sec_off + 0x110)[0]
    image_size   = struct.unpack_from(">I", d, sec_off + 0x04)[0]
    if 0x000003FF not in opt:
        # No file-format header: basefile is stored raw (uncompressed, unencrypted)
        print(f"file_format: (none) -> raw basefile; load=0x{load_address:08X} image_size=0x{image_size:X}")
        out = d[header_size: header_size + image_size]
        print(f"raw image: {len(out)} bytes")
        return bytes(out), load_address

    ff = opt[0x000003FF]
    info_size, enc_type, comp_type = struct.unpack_from(">IHH", d, ff)
    print(f"file_format: info_size=0x{info_size:X} enc_type={enc_type} comp_type={comp_type} "
          f"load=0x{load_address:08X} image_size=0x{image_size:X}")

    if comp_type == 1:
        # BASIC compression block list: pairs (data_size, zero_size)
        blocks = []
        q = ff + 8
        n = (info_size - 8) // 8
        for _ in range(n):
            ds, zs = struct.unpack_from(">II", d, q); q += 8
            blocks.append((ds, zs))
        total_comp = sum(b[0] for b in blocks)
        total_out  = sum(b[0] + b[1] for b in blocks)
        print(f"basic blocks: {len(blocks)}  compressed={total_comp}  decompressed={total_out}")
    elif comp_type == 0:
        total_comp = image_size
        blocks = [(image_size, 0)]
        print("no compression")
    else:
        raise SystemExit(f"compression_type {comp_type} (LZX) not handled by this inspector; use XenonRecomp")

    data = d[header_size: header_size + total_comp]
    if enc_type == 1:
        wrapped = d[sec_off + 0x150: sec_off + 0x160]
        session_key = AES.new(RETAIL_KEY, AES.MODE_ECB).decrypt(wrapped)
        if len(data) % 16:
            data += b"\x00" * (16 - len(data) % 16)
        data = AES.new(session_key, AES.MODE_CBC, iv=b"\x00" * 16).decrypt(data)

    out = bytearray(); pos = 0
    for ds, zs in blocks:
        out += data[pos:pos + ds]; pos += ds
        out += b"\x00" * zs
    print(f"image: {len(out)} bytes; load base 0x{load_address:08X}")
    return bytes(out), load_address

def find_stubs(img, base):
    print("\n--- register save/restore stubs (found VA vs issue #34) ---")
    ok = True
    for name, (pat, expect) in PATTERNS.items():
        idx = img.find(pat)
        if idx < 0:
            print(f"  {name:14} NOT FOUND (pattern {pat.hex()})"); ok = False; continue
        va = base + idx
        # verify uniqueness of the pattern (should be exactly one for reliability)
        second = img.find(pat, idx + 1)
        uniq = "" if second < 0 else f"  (WARN: also at 0x{base+second:08X})"
        match = "OK" if va == expect else f"DIFF (issue#34={hex(expect)})"
        print(f"  {name:14} 0x{va:08X}  [{match}]{uniq}")
        if va != expect: ok = False
    return ok

if __name__ == "__main__":
    xex = sys.argv[1] if len(sys.argv) > 1 else r"D:\Projects\Tekken Tag Tournament 2 (USA, Europe)\re\Default.xex"
    img, base = unpack(xex)
    outdir = r"D:\Projects\Tekken Tag Tournament 2 (USA, Europe)\re\decrypted"
    os.makedirs(outdir, exist_ok=True)
    stem = os.path.splitext(os.path.basename(xex))[0]
    outp = os.path.join(outdir, stem + ".bin")
    with open(outp, "wb") as g:
        g.write(img)
    print(f"wrote {outp}")
    find_stubs(img, base)
