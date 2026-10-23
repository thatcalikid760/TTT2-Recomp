import re, pathlib, sys
target = sys.argv[1]
for f in sorted(pathlib.Path("re/ppc").glob("ppc_recomp.*.cpp")):
    text = f.read_text(errors="ignore")
    i = text.find("PPC_FUNC_IMPL(__imp__sub_%s)" % target)
    if i >= 0:
        j = text.find("PPC_FUNC_IMPL(__imp__sub_", i + 10)
        body = text[i:j]
        # keep only the original asm comments for compactness
        out = [l.strip()[3:] for l in body.splitlines() if l.strip().startswith("// ") or l.strip().startswith("loc_")]
        print(f.name, len(out), "lines")
        print("\n".join(out[:int(sys.argv[2]) if len(sys.argv) > 2 else 400]))
        break
