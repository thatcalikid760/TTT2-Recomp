"""Splice regenerated code for specific instructions into the existing generated PPC sources.

The checked-in re/ppc/*.cpp carry hand-added diagnostics (probes), so a recompiler fix is
applied surgically: for every instruction block in re/ppc whose code is a placeholder trap
(`__builtin_debugtrap();`) and whose mnemonic is in --mnemonics, the block (from its
`// <instruction>` comment up to the next instruction comment or label) is replaced by the
block for the same instruction in the same function from a fresh regeneration (re/ppc_regen).
With --all-differing, every block of those mnemonics whose regenerated code differs is replaced
(e.g. barriers that used to emit nothing). Only files that change are rewritten. Prints a
summary; --dry-run writes nothing.
    python port/tools/splice_recomp_blocks.py --old re/ppc --new re/ppc_regen --mnemonics vpkd3d128 vupkd3d128
    python port/tools/splice_recomp_blocks.py --old re/ppc --new re/ppc_regen --mnemonics sync lwsync eieio --all-differing
"""
import argparse
import re
from pathlib import Path

FUNC = re.compile(r"PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\)")
INSN = re.compile(r"\s*// ([a-z][a-z0-9.+-]*)\b")
LABEL = re.compile(r"(loc_[0-9A-F]+):")


def blocks(lines):
    """Yield (function, instruction text, occurrence, start, end) for every instruction block."""
    func, seen = None, {}
    starts = []
    for i, line in enumerate(lines):
        m = FUNC.match(line)
        if m:
            func, seen = m.group(1), {}
        if INSN.match(line) and func:
            text = line.strip()
            n = seen.get(text, 0)
            seen[text] = n + 1
            starts.append((func, text, n, i))
    for k, (func, text, n, i) in enumerate(starts):
        end = i + 1
        while end < len(lines) and not INSN.match(lines[end]) and not LABEL.match(lines[end]) \
                and not lines[end].startswith("}") and not FUNC.match(lines[end]):
            end += 1
        yield func, text, n, i, end


def preambles(lines):
    """function -> (index of its PPC_FUNC_PROLOGUE line, declaration lines that follow it)."""
    out, func = {}, None
    for i, line in enumerate(lines):
        m = FUNC.match(line)
        if m:
            func = m.group(1)
            continue
        if func and line.strip() == "PPC_FUNC_PROLOGUE();":
            # Declarations sit between the prologue and the first instruction, possibly
            # around hand-added probe lines.
            decls, j = [], i + 1
            while j < len(lines) and not INSN.match(lines[j]) and not LABEL.match(lines[j]) and not lines[j].startswith("}"):
                if lines[j].strip().endswith("{};"):
                    decls.append(lines[j].rstrip("\r"))
                j += 1
            out[func] = (i, decls)
            func = None
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--mnemonics", nargs="+", required=True)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--all-differing", action="store_true")
    args = ap.parse_args()
    wanted = set(args.mnemonics)
    new_index, new_decls = {}, {}
    for path in sorted(Path(args.new).glob("ppc_recomp.*.cpp")):
        lines = path.read_bytes().decode("utf-8").split("\n")
        for func, text, n, i, end in blocks(lines):
            if INSN.match(lines[i]).group(1) in wanted:
                new_index[(func, text, n)] = lines[i:end]
        for func, (_, decls) in preambles(lines).items():
            new_decls[func] = decls
    total = files = 0
    missing = []
    for path in sorted(Path(args.old).glob("ppc_recomp.*.cpp")):
        raw = path.read_bytes().decode("utf-8")
        crlf = "\r\n" in raw
        lines = raw.split("\n")
        edits = []
        for func, text, n, i, end in blocks(lines):
            if INSN.match(lines[i]).group(1) not in wanted:
                continue
            trap = any("__builtin_debugtrap" in l for l in lines[i:end])
            if not trap and not args.all_differing:
                continue
            repl = new_index.get((func, text, n))
            if repl is None:
                missing.append((path.name, func, text))
                continue
            if any("__builtin_debugtrap" in l for l in repl):
                continue                      # still unimplemented in the recompiler
            if [l.rstrip("\r") for l in lines[i:end]] == [l.rstrip("\r") for l in repl]:
                continue                      # already current
            edits.append((i, end, repl, func))
        if not edits:
            continue
        # The regenerated code may use locals (temp, vTemp) the function never declared while
        # its only use was a trap: add the regenerated function's missing declarations.
        old_pre = preambles(lines)
        for func in {e[3] for e in edits}:
            at, have = old_pre[func]
            missing_decls = [d for d in new_decls.get(func, []) if d not in have]
            if missing_decls:
                edits.append((at + 1, at + 1, missing_decls, func))
        for i, end, repl, _ in sorted(edits, key=lambda e: (e[0], e[1]), reverse=True):
            # Match the target file's line endings (lines keep a trailing \r when it is CRLF).
            lines[i:end] = [l.rstrip("\r") + ("\r" if crlf else "") for l in repl]
        total += len(edits)
        files += 1
        if not args.dry_run:
            path.write_bytes("\n".join(lines).encode("utf-8"))   # byte-exact outside the edits
    print(f"spliced {total} blocks in {files} files" + (" (dry run)" if args.dry_run else ""))
    for m in missing[:20]:
        print("no regenerated block for", m)


if __name__ == "__main__":
    main()
