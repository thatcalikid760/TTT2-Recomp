"""Report which ALU constant rows each captured shader reads.

The runtime writes every distinct shader the game loads to port/shaders/{vs,ps}_<hash>.bin as
raw Xenos microcode. Decoding the control flow and the ALU source operands offline answers
"which constants does this draw actually need" without running the game - which is the question
the black-screen investigation keeps coming back to.

Bit layouts mirror gpu_render.cpp's Cf/Alu accessors exactly; keep them in step.
"""
import glob
import os
import struct
import sys

VEC = ["ADD","MUL","MAX","MIN","SEQ","SGT","SGE","SNE","FRC","TRUNC","FLOOR","MAD","CNDEQ",
       "CNDGE","CNDGT","DP4","DP3","DP2ADD","CUBE","MAX4","SETP_EQ_PUSH","SETP_NE_PUSH",
       "SETP_GT_PUSH","SETP_GE_PUSH","KILL_EQ","KILL_GT","KILL_GE","KILL_NE","DST","MAXA"]


def cf_pair(g, i):
    a_lo, a_hi = g[i], g[i + 1] & 0xFFFF
    b_lo = ((g[i + 1] >> 16) | ((g[i + 2] << 16) & 0xFFFFFFFF)) & 0xFFFFFFFF
    b_hi = g[i + 2] >> 16
    return (a_lo, a_hi), (b_lo, b_hi)


def decode(path):
    data = open(path, "rb").read()
    g = list(struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4]))
    execs, i = [], 0
    first_alu = len(g)
    while i + 2 < len(g) and i * 4 < first_alu * 4:
        for lo, hi in cf_pair(g, i):
            op = (hi >> 12) & 0xF
            if op in (1, 2, 3, 4, 5, 6, 13, 14):          # any Exec form
                addr, cnt = lo & 0xFFF, (lo >> 12) & 7
                execs.append((addr, cnt))
                first_alu = min(first_alu, addr * 3)
        i += 3
    reads, lines = set(), []
    for addr, cnt in execs:
        for k in range(cnt):
            o = (addr + k) * 3
            if o + 2 >= len(g):
                continue
            w0, w1, w2 = g[o], g[o + 1], g[o + 2]
            if (w0 & 0x1F) == 0 and o < first_alu:        # inside the CF block; skip
                continue
            opc = (w2 >> 24) & 0x1F
            srcs = []
            for n in (1, 2, 3):
                reg = (w2 >> ((3 - n) * 8)) & 0xFF
                gpr = (w2 >> (29 + (3 - n))) & 1
                if gpr:
                    srcs.append("r%d" % (reg & 0x3F))
                else:
                    srcs.append("c%d" % reg)
                    reads.add(reg)
            dst = ("oExp%d" % (w0 & 0x3F)) if (w0 >> 15) & 1 else "r%d" % (w0 & 0x3F)
            lines.append("    %-6s %-8s <- %s" % (VEC[opc] if opc < 30 else "op%d" % opc,
                                                  dst, ", ".join(srcs)))
    return reads, lines, execs


def main() -> int:
    pats = sys.argv[1:] or sorted(glob.glob("port/shaders/*.bin"))
    for p in pats:
        reads, lines, execs = decode(p)
        print("%s  execs=%s  constants read: %s"
              % (os.path.basename(p), execs, sorted(reads)))
        for l in lines:
            print(l)
    return 0


if __name__ == "__main__":
    sys.exit(main())
