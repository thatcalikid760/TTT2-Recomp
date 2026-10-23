#!/usr/bin/env python3
"""Harvest `// ERROR <addr>` branch targets from the generated re/ppc sources and add them
as explicit [main] functions entries in re/recomp/tekken.toml.

Those ERROR sites are branch/switch targets that fall inside a function but are not a known
function symbol (mis-split boundaries, XenonRecomp issue #34). At runtime an ERROR case is a
silent no-op return that corrupts guest register state. Declaring each target as a function
of its own (from the target to the end of the enclosing function) makes the recompiler emit
a real tail-call instead.

Usage: python port/tools/gen_missing_functions.py   (from C:/ttt2)
Then re-run XenonRecomp and rebuild. Iterate until the `// ERROR` count is 0 (adding
fragment functions can surface new out-of-range targets inside the fragments).
"""
import re
import sys
from bisect import bisect_right
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PPC = ROOT / "re" / "ppc"
TOML = ROOT / "re" / "recomp" / "tekken.toml"
EXTRA = ROOT / "re" / "recomp" / "extra_functions.txt"   # runtime-discovered targets, one hex addr/line
BEGIN = "# --- auto-generated missing-function boundaries (gen_missing_functions.py) ---"
END = "# --- end auto-generated ---"


def main():
    targets = set()
    starts = set()
    err_re = re.compile(r"// ERROR ([0-9A-F]{8})")
    fn_re = re.compile(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)")
    for path in sorted(PPC.glob("ppc_recomp.*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        targets.update(int(m, 16) for m in err_re.findall(text))
        starts.update(int(m, 16) for m in fn_re.findall(text))
    print(f"{len(targets)} distinct ERROR targets, {len(starts)} known functions")

    # Addresses reported at runtime ([runtime] UNMAPPED indirect call) live in EXTRA.
    if EXTRA.exists():
        for line in EXTRA.read_text().splitlines():
            line = line.split("#")[0].strip()
            if line:
                targets.add(int(line, 16))
        print(f"{len(targets)} targets after merging {EXTRA.name}")

    ordered = sorted(starts)
    entries = []
    skipped = 0
    for t in sorted(targets):
        i = bisect_right(ordered, t)
        if i == len(ordered):
            size = 0x100
        else:
            size = ordered[i] - t
        if t in starts or size <= 0:
            skipped += 1
            continue
        entries.append((t, size))
    print(f"{len(entries)} function entries ({skipped} skipped)")

    toml = TOML.read_text(encoding="utf-8")
    # Union with any previous auto-generated block: once a target became a real function,
    # it stops producing ERROR lines, but its entry must SURVIVE future regenerations.
    merged = {t: s for t, s in entries}
    if BEGIN in toml:
        old_block = toml[toml.index(BEGIN): toml.index(END)]
        for m in re.finditer(r"address = 0x([0-9A-F]{8}), size = 0x([0-9A-F]+)", old_block, re.I):
            t, s = int(m.group(1), 16), int(m.group(2), 16)
            merged.setdefault(t, s)
        pre = toml[: toml.index(BEGIN)]
        post = toml[toml.index(END) + len(END):]
        toml = pre + post

    # RE-SIZE every fragment against ORIGINAL function starts only. A fragment must span
    # to the end of its ENCLOSING original function: sizing it up to the next fragment
    # label cuts the range mid-flow, and execution silently falls off the end (state
    # corruption). Overlapping fragments are fine - each compiles the full remaining tail.
    original = sorted(s for s in starts if s not in merged)
    for t in list(merged.keys()):
        i = bisect_right(original, t)
        merged[t] = (original[i] - t) if i < len(original) else 0x100

    entries = sorted(merged.items())
    print(f"{len(entries)} entries after merge; sizes vs {len(original)} original functions")
    lines = [BEGIN, "functions = ["]
    for t, size in entries:
        lines.append(f"    {{ address = 0x{t:08X}, size = 0x{size:X} }},")
    lines.append("]")
    lines.append(END)
    block = "\n".join(lines) + "\n"
    toml = toml.rstrip() + "\n\n" + block
    TOML.write_text(toml, encoding="utf-8")
    print(f"wrote {len(entries)} entries to {TOML}")


if __name__ == "__main__":
    main()
