"""Find recompiled functions that build a constant address in a range (lis + addi/ori/lwz/... pairs).

Scans re/ppc/ppc_recomp.*.cpp (the XenonRecomp output keeps each instruction as a `// mnemonic`
comment). Within each function it remembers `lis rX,hi` values and resolves later
`addi rY,rX,lo` / `ori` / `lwz|stw|lbz|lhz|... rY,lo(rX)` uses, so compiler-hoisted high halves
are still matched; an addi result is itself tracked, so pool-base + offset chains resolve too
(approximate: no control flow, a register is trusted until the next addi overwrites it). Reports function, line and resolved address.

  python port/tools/find_addr_refs2.py 0x8202ED00 0x8202EF60
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FUNC = re.compile(r"^PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\)")
LIS = re.compile(r"^\s*// lis (r\d+),(-?\d+)$")
ADDI = re.compile(r"^\s*// (?:addi|addic) (r\d+),(r\d+),(-?\d+)$")
ORI = re.compile(r"^\s*// ori (r\d+),(r\d+),(\d+)$")
MEM = re.compile(r"^\s*// (?:lwz|lhz|lha|lbz|stw|sth|stb|lfs|lfd|stfs|stfd|lwzu|stwu) (?:[rf]\d+),(-?\d+)\((r\d+)\)$")


def main():
    lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
    for path in sorted((ROOT / "re/ppc").glob("ppc_recomp.*.cpp"), key=lambda p: int(p.stem.split(".")[1])):
        func, lis = None, {}
        with path.open(errors="replace") as f:
            for n, line in enumerate(f, 1):
                m = FUNC.match(line)
                if m:
                    func, lis = m.group(1), {}
                    continue
                if not func or "//" not in line:
                    continue
                m = LIS.match(line)
                if m:
                    lis[m.group(1)] = (int(m.group(2)) & 0xFFFF) << 16
                    continue
                addr = None
                m = ADDI.match(line)
                if m and m.group(2) in lis:
                    addr = (lis[m.group(2)] + int(m.group(3))) & 0xFFFFFFFF
                    lis[m.group(1)] = addr          # propagate: rY now holds a known constant
                elif m:
                    lis.pop(m.group(1), None)
                else:
                    m2 = ORI.match(line)
                    if m2 and m2.group(2) in lis:
                        addr = lis[m2.group(2)] | int(m2.group(3))
                    else:
                        m3 = MEM.match(line)
                        if m3 and m3.group(2) in lis:
                            addr = (lis[m3.group(2)] + int(m3.group(1))) & 0xFFFFFFFF
                if addr is not None and lo <= addr < hi:
                    print(f"{func} {path.name}:{n} {addr:08X} {line.strip()}")


if __name__ == "__main__":
    main()
