import re, sys, bisect
text = open("port/build-o1/tekken_recomp.map", errors="ignore").read()
pref = int(re.search(r"Preferred load address is\s+([0-9A-Fa-f]+)", text).group(1), 16)
syms = []
for name, value in re.findall(r"^\s*[0-9A-Fa-f]{4}:[0-9A-Fa-f]+\s+(\S+)\s+([0-9A-Fa-f]{16})\s", text, re.M):
    syms.append((int(value, 16) - pref, name))
syms.sort()
keys = [s[0] for s in syms]
for rva in sys.argv[1:]:
    r = int(rva, 16)
    k = bisect.bisect_right(keys, r) - 1
    print("%s -> %s +0x%X" % (rva, syms[k][1], r - syms[k][0]))
