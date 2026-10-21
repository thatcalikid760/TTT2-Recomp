"""Inventory Xenos microcode features used by captured shaders (port/shaders/*.bin).

Decodes control flow, fetches and ALU words with the same bit layouts as the CPU
interpreter (gpu_render.cpp) and reports, per stage, which features occur and in
how many shaders - to decide what the interpreter / hardware translator must add.

  python port/tools/ucode_features.py [--stage vs|ps] [--list FEATURE]
"""
import argparse
import collections
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
CF_NAMES = {0: "NOP", 1: "EXEC", 2: "EXEC_END", 3: "COND_EXEC", 4: "COND_EXEC_END", 5: "COND_EXEC_PRED",
            6: "COND_EXEC_PRED_END", 7: "LOOP_START", 8: "LOOP_END", 9: "COND_CALL", 10: "RETURN",
            11: "COND_JMP", 12: "ALLOC", 13: "COND_EXEC_PRED_CLEAN", 14: "COND_EXEC_PRED_CLEAN_END",
            15: "MARK_VS_FETCH_DONE"}
VEC = ["ADD", "MUL", "MAX", "MIN", "SEQ", "SGT", "SGE", "SNE", "FRC", "TRUNC", "FLOOR", "MAD", "CNDEQ", "CNDGE",
       "CNDGT", "DP4", "DP3", "DP2ADD", "CUBE", "MAX4", "SETP_EQ_PUSH", "SETP_NE_PUSH", "SETP_GT_PUSH",
       "SETP_GE_PUSH", "KILL_EQ", "KILL_GT", "KILL_GE", "KILL_NE", "DST", "MAXA"]
SCA = ["ADDS", "ADDS_PREV", "MULS", "MULS_PREV", "MULS_PREV2", "MAXS", "MINS", "SEQS", "SGTS", "SGES", "SNES",
       "FRCS", "TRUNCS", "FLOORS", "EXP", "LOGC", "LOG", "RCPC", "RCPF", "RCP", "RSQC", "RSQF", "RSQ", "MAXAS",
       "MAXASF", "SUBS", "SUBS_PREV", "SETP_EQ", "SETP_NE", "SETP_GT", "SETP_GE", "SETP_INV", "SETP_POP",
       "SETP_CLR", "SETP_RSTR", "KILLS_EQ", "KILLS_GT", "KILLS_GE", "KILLS_NE", "KILLS_ONE", "SQRT", "UNK41",
       "MULSC0", "MULSC1", "ADDSC0", "ADDSC1", "SUBSC0", "SUBSC1", "SIN", "COS", "RETAIN_PREV"]
# Opcodes the interpreter (ExecAlu) does not implement.
VEC_MISSING = {"CUBE"}
SCA_MISSING = {"MULS_PREV2", "SETP_POP", "UNK41", "MULSC0", "MULSC1", "ADDSC0", "ADDSC1", "SUBSC0", "SUBSC1"}
FMT_SUPPORTED = {36, 37, 57, 38, 6, 2, 4, 3, 15, 10, 25, 26, 31, 32, 7}   # UnpackFormat cases


def cf_pair(words):
    g0, g1, g2 = words
    return [(g0, g1 & 0xFFFF), ((g1 >> 16) | ((g2 << 16) & 0xFFFFFFFF), g2 >> 16)]


def analyze(code):
    feats = set()
    n = len(code)
    if n < 3:
        return feats
    cf_end = n
    for off in range(0, n - 2, 3):
        if off >= cf_end:
            break
        for lo, hi in cf_pair(code[off:off + 3]):
            op = (hi >> 12) & 0xF
            if op in (1, 2, 3, 4, 5, 6, 13, 14) and (lo & 0xFFF):
                cf_end = min(cf_end, (lo & 0xFFF) * 3)
    for off in range(0, cf_end - 2, 3):
        for lo, hi in cf_pair(code[off:off + 3]):
            op = (hi >> 12) & 0xF
            feats.add("CF_" + CF_NAMES[op])
            if op not in (1, 2, 3, 4, 5, 6, 13, 14):
                continue
            addr, count, seq = lo & 0xFFF, (lo >> 12) & 7, (lo >> 16) & 0xFFF
            for i in range(count):
                idx = addr + i
                if (idx + 1) * 3 > n:
                    break
                w0, w1, w2 = code[idx * 3: idx * 3 + 3]
                if (seq >> (i * 2)) & 1:
                    if (w0 & 0x1F) == 0:
                        fmt = (w1 >> 16) & 0x3F
                        feats.add(f"VFETCH_FMT_{fmt}" + ("" if fmt in FMT_SUPPORTED else "_UNSUPPORTED"))
                        if (w1 >> 30) & 1:
                            feats.add("VFETCH_MINI")
                        if (w1 >> 14) & 1:
                            feats.add("VFETCH_SIGNED")
                        if (w1 >> 13) & 1:
                            feats.add("VFETCH_INTEGER")
                        if ((w1 << 2) & 0xFC000000):
                            feats.add("VFETCH_EXP_ADJUST")
                    else:
                        op = w0 & 0x1F
                        feats.add("TFETCH_OP_%d" % op if op != 1 else "TFETCH")
                        dim = (w2 >> 18) & 3
                        if dim:
                            feats.add("TFETCH_DIM_%d" % dim)
                        if w0 & (1 << 11) or w0 & (1 << 18):
                            feats.add("TFETCH_RELATIVE")
                        if w2 != 0x4000 and op == 1:
                            feats.add("TFETCH_LOD_OR_OFFSET_FIELDS")
                        # Filter overrides: 0 point, 1 linear, 2 base map, 3 use fetch constant.
                        feats.add("TFETCH_MAG_%d" % ((w1 >> 12) & 3))
                        feats.add("TFETCH_MIN_%d" % ((w1 >> 14) & 3))
                        feats.add("TFETCH_MIP_%d" % ((w1 >> 16) & 3))
                        if (w1 >> 28) & 1 or (w1 >> 29) & 1:
                            feats.add("TFETCH_COMP_OR_REG_LOD")
                else:
                    vop, sop = (w2 >> 24) & 0x1F, (w0 >> 26) & 0x3F
                    v = VEC[vop] if vop < len(VEC) else "V%d" % vop
                    s = SCA[sop] if sop < len(SCA) else "S%d" % sop
                    feats.add("V_" + v + ("_MISSING" if v in VEC_MISSING or vop >= len(VEC) else ""))
                    feats.add("S_" + s + ("_MISSING" if s in SCA_MISSING else ""))
                    if (w1 >> 31) & 1:
                        feats.add("CONST0_RELATIVE" + ("_AL" if not (w1 >> 29) & 1 else "_A0"))
                    if (w1 >> 30) & 1:
                        feats.add("CONST1_RELATIVE")
                    if (w0 >> 6) & 1 or (w0 >> 14) & 1 and not (w0 >> 15) & 1:
                        feats.add("DEST_RELATIVE")
                    for k in range(3):
                        sel = (w2 >> (29 + (2 - k))) & 1
                        reg = (w2 >> ((2 - k) * 8)) & 0xFF
                        if sel and (reg & 0x40):
                            feats.add("TEMP_SRC_RELATIVE")
                    if (w1 >> 28) & 1:
                        feats.add("PREDICATED")
                    if (w0 >> 15) & 1:
                        feats.add("EXPORT_%d" % (w0 & 0x3F))
    return feats


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--stage", choices=["vs", "ps"], default="vs")
    parser.add_argument("--list", help="print shaders using this feature")
    args = parser.parse_args()
    files = sorted((ROOT / "port/shaders").glob(args.stage + "_*.bin"))
    counts = collections.Counter()
    users = collections.defaultdict(list)
    for f in files:
        data = f.read_bytes()
        code = list(struct.unpack("<%dI" % (len(data) // 4), data[:len(data) // 4 * 4]))
        for feat in analyze(code):
            counts[feat] += 1
            users[feat].append(f.name)
    print(f"{len(files)} {args.stage} shaders")
    for feat, c in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0])):
        flag = " <-- " if ("MISSING" in feat or "UNSUPPORTED" in feat or feat in
                           ("CF_LOOP_START", "CF_LOOP_END", "CF_COND_CALL", "CF_RETURN", "CF_COND_JMP",
                            "TEMP_SRC_RELATIVE", "DEST_RELATIVE", "TFETCH_RELATIVE", "CONST1_RELATIVE")) else ""
        print(f"{c:5d}  {feat}{flag}")
    if args.list:
        print("\n".join(users.get(args.list, [])))


if __name__ == "__main__":
    main()
