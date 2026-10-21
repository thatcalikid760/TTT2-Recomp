"""Disassemble a captured Xenos shader (port/shaders/*.bin) for reading.

Bit layouts as in the CPU interpreter (gpu_render.cpp) and Xenia's ucode.h. Output is a
readable listing: control flow, vertex/texture fetches and paired vector+scalar ALU slots
with operand sources (rN temps, cN constants), swizzles, negation and write masks.

  python port/tools/ucode_disasm.py port/shaders/vs_0DC049A47C26C910.bin
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ucode_features import CF_NAMES, SCA, VEC, cf_pair  # noqa: E402

XYZW = "xyzw"


def swizzle(sw):
    # Each 2-bit field is relative to the identity component.
    return "".join(XYZW[((sw >> (i * 2)) + i) & 3] for i in range(4))


def mask(m):
    return "".join(XYZW[i] if m & (1 << i) else "_" for i in range(4))


def operand(index, w1, w2, abs_constants):
    sel = (w2 >> (31 - (index - 1))) & 1          # src1 bit31, src2 bit30, src3 bit29
    reg = (w2 >> (16 - (index - 1) * 8)) & 0xFF   # src1 16-23, src2 8-15, src3 0-7
    sw = (w1 >> (16 - (index - 1) * 8)) & 0xFF
    neg = (w1 >> (26 - (index - 1))) & 1
    if sel:
        name = "r%d" % (reg & 0x3F)
        if reg & 0x40:
            name = "r[%d+aL]" % (reg & 0x3F)
        if reg & 0x80:
            name = "|%s|" % name
    else:
        name = "c%d" % reg
        if abs_constants:
            name = "|%s|" % name
    s = swizzle(sw)
    return ("-" if neg else "") + name + ("" if s == "xyzw" else "." + s)


def alu(words):
    w0, w1, w2 = words
    vop, sop = (w2 >> 24) & 0x1F, (w0 >> 26) & 0x3F
    vdst, sdst = w0 & 0x3F, (w0 >> 8) & 0x3F
    export = (w0 >> 15) & 1
    vmask, smask = (w0 >> 16) & 0xF, (w0 >> 20) & 0xF
    absc = (w0 >> 7) & 1
    dst = ("o%d" if export else "r%d")
    pred = ""
    if (w1 >> 28) & 1:
        pred = "(%sp0) " % ("" if (w1 >> 27) & 1 else "!")
    rel = []
    if (w1 >> 31) & 1:
        rel.append("c0rel=%s" % ("a0" if (w1 >> 29) & 1 else "aL"))
    if (w1 >> 30) & 1:
        rel.append("c1rel")
    out = []
    vname = VEC[vop] if vop < len(VEC) else "V%d" % vop
    nsrc = 3 if vname in ("MAD", "CNDEQ", "CNDGE", "CNDGT", "DP2ADD") else 1 if vname in ("FRC", "TRUNC", "FLOOR", "MOVA", "MOV") else 2
    if vmask or not export:
        srcs = ", ".join(operand(i, w1, w2, absc) for i in range(1, nsrc + 1))
        out.append("%s%s %s.%s, %s%s" % (pred, vname, dst % vdst, mask(vmask), srcs,
                                       (" [" + " ".join(rel) + "]") if rel else ""))
    sname = SCA[sop] if sop < len(SCA) else "S%d" % sop
    if smask:
        # Scalar ops read src3 (and, for the *_C constant forms, a constant in src3's slot).
        out.append("%s  + %s %s.%s, %s" % (pred, sname, (dst % sdst), mask(smask), operand(3, w1, w2, absc)))
    return out


def fetch(words):
    w0, w1, w2 = words
    op = w0 & 0x1F
    if op == 0:
        fmt, stride, offset = (w1 >> 16) & 0x3F, w2 & 0xFF, (w2 >> 8) & 0x7FFFFF
        return "VFETCH%s r%d.%s <- r%d.%s const%d fmt=%d stride=%d offset=%d%s%s" % (
            "_MINI" if (w1 >> 30) & 1 else "", (w0 >> 12) & 0x3F, "".join(
                "xyzw01_?"[(w1 >> (i * 3)) & 7] for i in range(4)), (w0 >> 5) & 0x3F,
            XYZW[(w0 >> 30) & 3], (w0 >> 20) & 0x1F, fmt, stride, offset,
            " signed" if (w1 >> 14) & 1 else "", " int" if (w1 >> 13) & 1 else "")
    return "TFETCH op%d r%d <- r%d tex%d dim=%d" % (op, (w0 >> 12) & 0x3F, (w0 >> 5) & 0x3F,
                                                  (w0 >> 20) & 0x1F, (w2 >> 14) & 3)


def main():
    data = Path(sys.argv[1]).read_bytes()
    code = list(struct.unpack("<%dI" % (len(data) // 4), data))
    n = len(code)
    cf_end = n
    for off in range(0, n - 2, 3):
        if off >= cf_end:
            break
        for lo, hi in cf_pair(code[off:off + 3]):
            if (hi >> 12) & 0xF in (1, 2, 3, 4, 5, 6, 13, 14) and lo & 0xFFF:
                cf_end = min(cf_end, (lo & 0xFFF) * 3)
    for off in range(0, cf_end - 2, 3):
        for lo, hi in cf_pair(code[off:off + 3]):
            op = (hi >> 12) & 0xF
            print("CF %s lo=%04X hi=%04X" % (CF_NAMES[op], lo & 0xFFFF, hi & 0xFFFF))
            if op not in (1, 2, 3, 4, 5, 6, 13, 14):
                continue
            addr, count, seq = lo & 0xFFF, (lo >> 12) & 7, (lo >> 16) & 0xFFF
            for i in range(count):
                idx = addr + i
                if (idx + 1) * 3 > n:
                    break
                words = code[idx * 3: idx * 3 + 3]
                lines = [fetch(words)] if (seq >> (i * 2)) & 1 else alu(words)
                for k, line in enumerate(lines):
                    print("  %3d  %s" % (idx, line) if k == 0 else "       %s" % line)


if __name__ == "__main__":
    main()
