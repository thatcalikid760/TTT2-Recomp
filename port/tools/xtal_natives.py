"""List the Xtal natives a TTT2 registration function defines: name, implementation, signature.

Registration functions (e.g. sub_823756A8 options, sub_8241FCD0 windows, sub_82572618 menus)
repeat one pattern per native: sub_82A802F0(&native, signatureDescriptor, &impl) builds the
native object, sub_82A6F5E8 interns the name (pool base register + offset, explicit length),
sub_822501A0(lib, &name, &native) defines it. This replays the function's lis/addi/mr/stw
constants (no control flow) to recover each triple. Same descriptor = same C signature, e.g.
0x8203624C setter(int), 0x8205B9A4 getter -> int, 0x82001348 void().

  python port/tools/xtal_natives.py 823756A8 [8241FCD0 ...]
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / "re/decrypted/Default_patched_tu3.bin"


def cstr(data, a):
    o = a - 0x82000000
    if not 0 <= o < len(data):
        return f"?{a:08X}"
    return data[o:data.index(b"\0", o)].decode(errors="replace")


def scan(func, data):
    want = f"PPC_FUNC_IMPL(__imp__sub_{func.upper()})"
    for path in (ROOT / "re/ppc").glob("ppc_recomp.*.cpp"):
        lines = path.read_text(errors="replace").splitlines()
        hit = [i for i, l in enumerate(lines) if want in l]
        if not hit:
            continue
        regs, fn, desc = {}, None, None
        for l in lines[hit[0] + 1:]:
            if l.startswith("PPC_FUNC_IMPL"):
                break
            m = re.match(r"\s*// (\w+) (.*)", l)
            if not m:
                continue
            op, a = m.group(1), [x.strip() for x in m.group(2).split(",")]
            if op == "bl" and a[0] == "0x82a6f5e8" and "r4" in regs:
                print(f"{cstr(data, regs['r4']):40s} impl={fn or 0:08X} desc={desc or 0:08X}")
                continue
            if op == "lis":
                regs[a[0]] = (int(a[1]) & 0xFFFF) << 16
            elif op == "addi" and a[1] in regs:
                regs[a[0]] = (regs[a[1]] + int(a[2])) & 0xFFFFFFFF
            elif op == "addi":
                regs.pop(a[0], None)
            elif op == "mr" and a[1] in regs:
                regs[a[0]] = regs[a[1]]
            elif op == "stw" and a[0] in regs and "(r1)" in a[1]:
                fn = regs[a[0]]
            elif op == "bl" and a[0] == "0x82a802f0":
                desc = regs.get("r4")
        return
    print(f"sub_{func} not found")


def main():
    data = IMAGE.read_bytes()
    for f in sys.argv[1:]:
        print(f"== sub_{f.upper()}")
        scan(f, data)


if __name__ == "__main__":
    main()
