#!/usr/bin/env python3
"""Restore measured switches that XenonAnalyse missed.

Run after XenonAnalyse and before XenonRecomp. Without these tables a bctr
becomes an indirect host call to an interior instruction; an unmapped target
returns without restoring the guest stack or saved registers. Signatures and
table digests pin the recovered maps to the measured TU3 image.
"""
from pathlib import Path
import hashlib
import re

ROOT = Path(__file__).resolve().parents[2]
IMAGE = ROOT / "re/decrypted/Default_patched_tu3.bin"
TABLES = ROOT / "re/recomp/jump_tables.toml"
IMAGE_BASE = 0x82000000
BEGIN = "# --- measured unbounded switches (gen_missing_switches.py) ---"
END = "# --- end measured unbounded switches ---"

# start, index register, table address, entries, element bytes, offset shift,
# case base (zero means absolute addresses), instruction (length, SHA256), table SHA256.
SWITCHES = [
    (0x827E472C, 18, 0x827E4744, 21, 4, 0, 0,
     (24, "f0dac1c617cdc48047ca3b161026bda5f349b5f715c5080ba37a2f95947d4cd3"),
     "2cd3ed9b223ed0eb171dcb12884397bc5385499a5e94af1bf68a06ab7f923663"),
    # Inferred bytecode domain 0..84. Entry 84 is 0x0080 and targets the valid
    # one-byte-advance path at 82A6C560. A zero halfword and the referenced
    # XRE1024 string follow. The producer's maximum opcode is not yet proven;
    # do not interpret the following strings/vtables as 256 handler offsets.
    (0x82A6C4B8, 11, 0x82171940, 85, 2, 0, 0x82A6C4E0,
     (40, "40c2ab1caba5bf599bfe8a7033c89e0f8d231836650385a759a4b8d86c399221"),
     "1a0374e06d610fce684d2837b944e269c498b17a732e1a5a0d4254d57ae92d3c"),
    # The following data starts with the separate string 'line'.
    (0x82A99DCC, 11, 0x821824B0, 28, 2, 0, 0x82A99DF4,
     (40, "a3728046234c5f82ca0db36dfa0e4a9c95032f813cff7b6776831a0bac2a4178"),
     "6b933742d6c2f55efcc59034695930153108394082b367115396ce339e40ef42"),
    # Type values 12..70. Zero bytes 12..46 are real cases; the five zeros
    # after case 58 align the following, separately referenced halfword table.
    (0x82AA8CE4, 10, 0x82186D68, 59, 1, 2, 0x82AA8D0C,
     (40, "934c663933aef1169e033a6d65eeda6090a46afda132b559a318942081dfe9b2"),
     "2eb3eaaf5179660cb5bf601fea5ac86781745788ba1e4a3e43077ca0edfec0e0"),
    # The caller predicate sub_82AA79A8 checks the type is within 24..35.
    (0x82ABD354, 11, 0x82187890, 12, 1, 0, 0x82ABD37C,
     (40, "5d0c933cfa6cafc9942f36d5feefd0856e8215653b745557c8ad6bd728fc4a94"),
     "3884c4a00c2539bfbf14b8d393d5a982722dc185ba83193f21f8e491176b72a3"),
    (0x82AC9F08, 17, 0x82AC9F20, 29, 4, 0, 0,
     (24, "e5e9ef340b6c8c8029e0abdb75dd30fa1596434cbfe20d7d0f6af9686009de4a"),
     "aeb3e155ed0fa06f0dda50ff8ecb139202aa5fc35733a45d31e23468c1bde063"),
]


def main():
    image = IMAGE.read_bytes()
    text = TABLES.read_text(encoding="utf-8")
    if BEGIN in text:
        before, rest = text.split(BEGIN, 1)
        _, after = rest.split(END, 1)
        text = before.rstrip() + after
    block = [BEGIN]
    for start, reg, table, count, width, shift, case_base, sig, digest in SWITCHES:
        signature_size, signature_digest = sig
        off = start - IMAGE_BASE
        if hashlib.sha256(image[off:off + signature_size]).hexdigest() != signature_digest:
            raise SystemExit(f"Switch {start:08X} instruction signature changed; re-derive it")
        raw = image[table - IMAGE_BASE:table - IMAGE_BASE + count * width]
        if hashlib.sha256(raw).hexdigest() != digest:
            raise SystemExit(f"Switch {start:08X} table contents changed; re-derive it")
        labels = [case_base + (int.from_bytes(raw[i:i + width], "big") << shift)
                  for i in range(0, len(raw), width)]
        if not all(0x82250000 <= label < 0x82DB848C and label % 4 == 0 for label in labels):
            raise SystemExit(f"Switch {start:08X} contains an invalid code target")
        if re.search(rf"base\s*=\s*0x{start:08X}\b", text, re.I):
            raise SystemExit(f"Switch {start:08X} already exists outside the managed block; review it")
        block.extend([f"# Table {table:08X}: {count} entries of {width} bytes.",
                      "[[switch]]", f"base = 0x{start:08X}", f"r = {reg}",
                      f"default = 0x{labels[0]:08X}", "labels = ["])
        block.extend(f"    0x{label:08X}," for label in labels)
        block.extend([
            "]", "",
        ])
        print(f"Restored switch {start:08X}: {len(labels)} labels, validated against the image")
    block.append(END)
    TABLES.write_text(text.rstrip() + "\n\n" + "\n".join(block) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
