"""Extract case blocks for a set of PPC_INST ids from a fork's recompiler.cpp,
dedupe fallthrough groups, and flag any labels that collide with base-implemented ids."""
import re, sys

BASE = r"C:\ttt2\tools\XenonRecomp\XenonRecomp\recompiler.cpp"
UNL  = r"C:\ttt2\tools\fork_unlimited\XenonRecomp\recompiler.cpp"

NEEDED = """VSLH BDZF VSRAH VSUBSHS VSPLTISH LHZU STHU STFSU LFSU VANDC VMAXSH SUBFZE
VPKSWSS128 VMINSH VCFPUXWS128 VSEL128 LBZUX LHZUX VCMPGTSH ADDC LFSUX VCMPGTSW ADDME
VADDSWS STDUX LHAU LWZUX STHUX STBUX VADDSBS LDUX VRLH VPKSWSS VCMPEQUH VAVGUH VCTUXS
DCBST VSRH VPKSWUS128 VNOR MULHDU STFSUX VSL STFDU VSUBUBM VSRAB VPKUHUS128 VPKUHUM
VPKSHSS MULHD VNOR128 CRORC CROR BDNZT VPKSHSS128""".split()

def implemented_ids(path):
    ids = set()
    for m in re.finditer(r"case PPC_INST_(\w+):", open(path, encoding="utf-8", errors="replace").read()):
        ids.add(m.group(1))
    return ids

def extract_blocks(path, needed):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    # find index of each 'case PPC_INST_X:' line
    blocks = {}  # start_of_run_label_index -> (labels, body_lines)
    n = len(lines)
    for i, ln in enumerate(lines):
        m = re.match(r"\s*case PPC_INST_(\w+):\s*$", ln)
        if not m:
            continue
        name = m.group(1)
        if name not in needed:
            continue
        # walk back to the first case label of this fallthrough run
        start = i
        while start - 1 >= 0 and re.match(r"\s*case PPC_INST_\w+:\s*$", lines[start-1]):
            start -= 1
        # walk forward to terminating 'break;' (or 'return;' or closing of block)
        end = i
        depth = 0
        while end < n:
            s = lines[end]
            depth += s.count("{") - s.count("}")
            if re.search(r"\bbreak;", s) and depth <= 0:
                break
            end += 1
        blocks[start] = (start, end)
    # dedupe by (start,end)
    uniq = sorted(set(blocks.values()))
    return lines, uniq

def main():
    base_ids = implemented_ids(BASE)
    lines, uniq = extract_blocks(UNL, set(NEEDED))
    found = set()
    out = []
    for start, end in uniq:
        block = lines[start:end+1]
        labels = re.findall(r"case PPC_INST_(\w+):", "\n".join(block))
        for l in labels:
            if l in NEEDED:
                found.add(l)
        collide = [l for l in labels if l in base_ids]
        hdr = f"// labels={labels}" + (f"   !!! COLLIDES WITH BASE: {collide}" if collide else "")
        out.append(hdr)
        out.extend(block)
        out.append("")
    print("\n".join(out))
    missing = sorted(set(NEEDED) - found)
    sys.stderr.write(f"\nExtracted {len(uniq)} unique blocks covering {len(found)}/{len(NEEDED)} needed ids\n")
    if missing:
        sys.stderr.write(f"NOT FOUND in fork: {missing}\n")

if __name__ == "__main__":
    main()
