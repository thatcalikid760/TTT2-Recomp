#!/usr/bin/env python3
"""Check the generated control-flow repairs before building the native port.

Run after XenonRecomp and gen_missing_functions.py. This reads generated code,
does not alter it, and rejects regression to unresolved interior bctr calls or
the truncated memset tail. The switch table domain limitations remain those
documented in gen_missing_switches.py; this check proves emission, not gameplay.
"""
from pathlib import Path
import re

from gen_missing_switches import IMAGE, IMAGE_BASE, ROOT, SWITCHES

PARENTS = {
    0x827E472C: 0x827E4368,
    0x82A6C4B8: 0x82A6C308,
    0x82A99DCC: 0x82A99DB0,
    0x82AA8CE4: 0x82AA8CB8,
    0x82ABD354: 0x82ABD328,
    0x82AC9F08: 0x82AC9D60,
}


def main():
    bodies = {}
    wanted = set(PARENTS.values()) | {0x82BD4320}
    errors = []
    for path in (ROOT / "re/ppc").glob("ppc_recomp.*.cpp"):
        text = path.read_text(encoding="utf-8")
        errors.extend(f"{path.name}: {target}" for target in re.findall(r"// ERROR ([0-9A-F]{8})", text))
        for match in re.finditer(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\) \{", text):
            address = int(match[1], 16)
            if address not in wanted:
                continue
            end = text.find("\n__attribute__", match.end())
            bodies[address] = text[match.end():end if end >= 0 else len(text)]
    image = IMAGE.read_bytes()
    total = 0
    for start, reg, table, count, width, shift, case_base, signature, _ in SWITCHES:
        parent = PARENTS[start]
        body = bodies.get(parent, "")
        if not body:
            errors.append(f"Missing parent function {parent:08X}")
            continue
        if re.search(r"// bctr\s*\n\s*PPC_CALL_INDIRECT_FUNC", body):
            errors.append(f"Unresolved bctr remains in parent {parent:08X}")
        raw = image[table - IMAGE_BASE:table - IMAGE_BASE + count * width]
        labels = [case_base + (int.from_bytes(raw[i:i + width], "big") << shift)
                  for i in range(0, len(raw), width)]
        switches = re.findall(rf"switch \(ctx\.r{reg}\.u64\) \{{(.*?)\n\t\}}", body, re.S)
        expected = {i: target for i, target in enumerate(labels)}
        found = False
        for switch in switches:
            cases = {int(index): int(target, 16) for index, target in re.findall(
                r"case (\d+):\s*(?:goto loc_|sub_)([0-9A-F]{8})(?:;|\(ctx, base\);)", switch)}
            if cases == expected:
                found = True
                branch_pc = start + signature[0] - 4
                diagnostic = f"PPCUnrecognizedSwitch(0x{branch_pc:08X}u, ctx.r{reg}.u64);"
                if diagnostic not in switch or "__builtin_unreachable" in switch:
                    errors.append(f"Switch {start:08X} still has an unsafe out-of-range default")
                break
        if not found:
            errors.append(f"Switch {start:08X} does not emit its exact {count}-case map in {parent:08X}")
        else:
            print(f"PASS switch {start:08X}: {count} cases in {parent:08X}, no unresolved bctr")
        total += count

    memset = bodies.get(0x82BD4320, "")
    stores = [f"PPC_STORE_U8(ctx.r6.u32 + {i}, ctx.r4.u8);" for i in range(3)]
    if not all(store in memset for store in stores):
        errors.append("memset 82BD4320 still lacks one of the three trailing-byte stores")
    elif not (memset.index(stores[0]) < memset.index(stores[1]) < memset.index(stores[2])):
        errors.append("memset trailing-byte stores are out of instruction order")
    elif memset.count("if (ctx.ctr.u32 == 0) return;") < 2:
        errors.append("memset tail lost its two conditional counter returns")
    else:
        print("PASS memset 82BD4320: all three trailing bytes and conditional-return fallthrough")
    if errors:
        raise SystemExit("FAIL:\n" + "\n".join(errors))
    print(f"PASS acceptance: {len(SWITCHES)} switches, {total} cases, full memset tail, zero ERROR sites")


if __name__ == "__main__":
    main()
