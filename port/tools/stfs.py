"""Read-only STFS (LIVE/PIRS/CON) container reader for Xbox 360 title updates.

Extracts files from a title-update package (the .xexp delta patch and the
data1XX.bin/.ofs archives). Read-only, defensive, no external deps.
Algorithm follows the well-documented py360/wxPirs block-addressing scheme.
Reusable by the installer to pull update payloads out of the user's own TU files.
"""
import struct, os, sys

class STFS:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        data = self.f.read(0x1000)  # header
        self.magic = data[:4]
        if self.magic not in (b"LIVE", b"PIRS", b"CON "):
            raise ValueError(f"not an STFS package: magic={self.magic!r}")
        self.header_size = struct.unpack_from(">I", data, 0x340)[0]
        self.content_type = struct.unpack_from(">I", data, 0x344)[0]
        self.media_id = struct.unpack_from(">I", data, 0x354)[0]
        self.version = struct.unpack_from(">I", data, 0x358)[0]
        self.base_version = struct.unpack_from(">I", data, 0x35C)[0]
        self.title_id = struct.unpack_from(">I", data, 0x360)[0]
        # STFS volume descriptor at 0x379
        self.desc_len = data[0x379]
        self.block_separation = data[0x37A]
        self.file_table_block_count = struct.unpack_from("<H", data, 0x37B)[0]
        self.file_table_block0 = int.from_bytes(data[0x37E:0x381], "little")
        self.total_alloc_blocks = struct.unpack_from(">I", data, 0x395)[0]
        # table size shift: read-only (1) vs read-write (0) determined by header size rounding
        if ((self.header_size + 0xFFF) & 0xF000) >> 0xC == 0xB:
            self.table_shift = 0
        else:
            self.table_shift = 1

    def _fix_block(self, block):
        """Map a data block number to a backing block number, skipping hash tables."""
        adj = 0
        if block >= 0xAA:
            adj += ((block // 0xAA) + 1) << self.table_shift
        if block >= 0x70E4:
            adj += ((block // 0x70E4) + 1) << self.table_shift
        return block + adj

    def _block_offset(self, block):
        return 0xC000 + (self._fix_block(block) << 0xC)

    def _read_block(self, block):
        self.f.seek(self._block_offset(block))
        return self.f.read(0x1000)

    def read_file_table(self):
        entries = []
        block = self.file_table_block0
        for _ in range(self.file_table_block_count):
            blk = self._read_block(block)
            for i in range(0, 0x1000, 0x40):
                rec = blk[i:i+0x40]
                name_len = rec[0x28] & 0x3F
                if name_len == 0:
                    continue
                name = rec[:name_len].decode("ascii", "replace")
                flags = rec[0x28]
                blocks_alloc = int.from_bytes(rec[0x29:0x2C], "little")
                start_block = int.from_bytes(rec[0x2F:0x32], "little")
                fsize = struct.unpack_from(">I", rec, 0x34)[0]
                is_dir = bool(flags & 0x80)
                path_indicator = struct.unpack_from(">h", rec, 0x32)[0]
                entries.append(dict(name=name, size=fsize, start=start_block,
                                    blocks=blocks_alloc, is_dir=is_dir, parent=path_indicator))
            # data blocks of the file table are contiguous via the hash chain; the
            # next file-table block is start+1 for these small packages. To be safe,
            # follow the allocation by reading consecutive blocks.
            block = self._next_block(block)
            if block == 0xFFFFFF or block == 0:
                break
        return entries

    def _hash_entry_offset(self, block):
        # L0 hash table for `block` sits just before its 0xAA-run
        table_index = block // 0xAA
        # backing block of the hash table
        base = (table_index * (0xAA + 1)) << 0
        # position of this block's 0x18-byte record inside the table
        return None  # not needed if we read files by contiguous allocation

    def _next_block(self, block):
        """Read the next-block pointer from the L0 hash table entry for `block`."""
        table_block = self._fix_block(block) - (block % 0xAA) - 1  # backing block of the L0 table
        off = (table_block << 0xC) + 0xC000 + (block % 0xAA) * 0x18
        self.f.seek(off + 0x15)  # next-block-number is 3 bytes at +0x15
        nb = self.f.read(3)
        if len(nb) < 3:
            return 0xFFFFFF
        return int.from_bytes(nb, "big")

    def read_file(self, entry):
        """Read a file's bytes by following its block chain."""
        out = bytearray()
        block = entry["start"]
        remaining = entry["size"]
        count = 0
        while remaining > 0 and block not in (0xFFFFFF, 0) and count <= entry["blocks"] + 1:
            blk = self._read_block(block)
            take = min(0x1000, remaining)
            out += blk[:take]
            remaining -= take
            block = self._next_block(block)
            count += 1
        return bytes(out)


def parse_xex_patch_descriptor(xexp_bytes):
    """Read source/target version out of an .xexp delta-patch XEX header (plaintext)."""
    d = xexp_bytes
    if d[:4] != b"XEX2":
        return None
    opt_count = struct.unpack_from(">I", d, 0x14)[0]
    opt = {}
    p = 0x18
    for _ in range(opt_count):
        k, v = struct.unpack_from(">II", d, p); p += 8
        opt[k] = v
    info = {"optional_keys": {hex(k): hex(v) for k, v in opt.items()}}
    # DELTA_PATCH_DESCRIPTOR key = 0x000005FF (offset into header)
    if 0x000005FF in opt:
        off = opt[0x000005FF]
        size = struct.unpack_from(">I", d, off)[0]
        tgt = struct.unpack_from(">I", d, off + 4)[0]
        src = struct.unpack_from(">I", d, off + 8)[0]
        def ver(v):
            return f"{(v>>28)&0xF}.{(v>>24)&0xF}.{(v>>8)&0xFFFF}.{v&0xFF}"
        info["patch_desc_size"] = size
        info["target_version_raw"] = hex(tgt)
        info["source_version_raw"] = hex(src)
        info["target_version"] = ver(tgt)
        info["source_version"] = ver(src)
    # EXECUTION_ID key = 0x00040006
    if 0x00040006 in opt:
        off = opt[0x00040006]
        media, version, base_version, title_id = struct.unpack_from(">IIII", d, off)
        info["exec_media_id"] = hex(media)
        info["exec_title_id"] = hex(title_id)
        info["exec_version_raw"] = hex(version)
        info["exec_base_version_raw"] = hex(base_version)
    return info


if __name__ == "__main__":
    upd_dir = r"D:\Projects\Tekken Tag Tournament 2 (USA, Europe)\Updates"
    out_root = r"D:\Projects\Tekken Tag Tournament 2 (USA, Europe)\re\extracted_tu"
    for fn in sorted(os.listdir(upd_dir)):
        path = os.path.join(upd_dir, fn)
        print(f"\n=== {fn} ===")
        try:
            s = STFS(path)
        except Exception as e:
            print(f"  skip: {e}")
            continue
        print(f"  title=0x{s.title_id:08X} media=0x{s.media_id:08X} content_type=0x{s.content_type:X}")
        print(f"  header_size=0x{s.header_size:X} table_shift={s.table_shift} ft_block0={s.file_table_block0} ft_blocks={s.file_table_block_count}")
        try:
            entries = s.read_file_table()
        except Exception as e:
            print(f"  file-table error: {e}")
            continue
        files = [e for e in entries if not e["is_dir"]]
        outdir = os.path.join(out_root, fn)
        os.makedirs(outdir, exist_ok=True)
        for e in files:
            print(f"    {e['size']:>10}  start={e['start']:<6} blocks={e['blocks']:<5} {e['name']}")
            try:
                data = s.read_file(e)
            except Exception as ex:
                print(f"        read error: {ex}")
                continue
            with open(os.path.join(outdir, e["name"]), "wb") as g:
                g.write(data)
            if e["name"].lower().endswith(".xexp"):
                info = parse_xex_patch_descriptor(data)
                if info:
                    for k in ("source_version", "target_version", "exec_title_id", "exec_media_id"):
                        if k in info:
                            print(f"        {k} = {info[k]}")
                    print(f"        opt_keys = {list(info['optional_keys'].keys())}")
