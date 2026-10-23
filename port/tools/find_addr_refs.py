import re, pathlib, sys
target = int(sys.argv[1], 16)
hi = (target + 0x8000) >> 16
lo = target - (hi << 16)
hi_s = hi - 0x10000 if hi >= 0x8000 else hi
pat_lo = re.compile(r"// (addi|ori|lwz|lbz|stw|li) r\d+,r\d+,%d\b" % lo)
for f in sorted(pathlib.Path("re/ppc").glob("ppc_recomp.*.cpp")):
    lines = f.read_text(errors="ignore").splitlines()
    func = None
    for i, l in enumerate(lines):
        m = re.match(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)", l)
        if m: func = m.group(1)
        if pat_lo.search(l):
            window = "\n".join(lines[max(0, i - 40):i])
            if "lis r" in window and ",%d" % hi_s in window:
                print("%s sub_%s line %d: %s" % (f.name, func, i + 1, l.strip()))
