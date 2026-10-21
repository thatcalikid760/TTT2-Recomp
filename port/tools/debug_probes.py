#!/usr/bin/env python3
"""Insert temporary fprintf probes into generated re/ppc/ppc_recomp.*.cpp functions.

Usage: python port/tools/debug_probes.py          (from C:/ttt2)
Re-run after any XenonRecomp regeneration (regen overwrites the probes).
Idempotent: skips a function if its probe marker is already present.

Each probe prints the first N entries to a function with selected register/memory
values, to trace the serialization-subsystem object creation chain (see
PROJECT_STATUS.md session 4: crash at sub_82A77FC8, object 0x80D20000).
Probes print only when the game runs with TTT2_PROBES=1 (ungated they wrote ~56k lines
into every log, the released game's included).
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PPC = ROOT / "re" / "ppc"

# (historical file, function, printf-format, args[, condition]) -- function names
# are resolved after regeneration because analyzer fixes can shift chunk boundaries.
# 'GR(expr)' means guest u32 read at expr;
# a 5th tuple element is a C++ condition the probe is wrapped in (print only when true).
PROBES = [
    # --- async file-io chain (nrfo truncation investigation) ---
    ("ppc_recomp.41.cpp", "sub_823BFFF8", "obj=%08X prefix=%08X name=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "ctx.r5.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.231.cpp", "sub_82ACBB58", "fd=%d lr=%08X",
     ["(int)ctx.r3.s32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.231.cpp", "sub_82ACC4B8", "fd=%d cb=%08X ctx=%08X lr=%08X",
     ["(int)ctx.r3.s32", "ctx.r4.u32", "ctx.r5.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.231.cpp", "sub_82ACC2B0", "fd=%d buf=%08X len=%u cb=%08X lr=%08X",
     ["(int)ctx.r3.s32", "ctx.r4.u32", "ctx.r5.u32", "ctx.r6.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.235.cpp", "sub_82AE3E40", "pkt=%08X fd=%d buf=%08X rem=%u tot=%u",
     ["ctx.r3.u32", "(int)GR(ctx.r3.u32+20)", "GR(ctx.r3.u32+28)", "GR(ctx.r3.u32+36)",
      "GR(ctx.r3.u32+44)"]),
    ("ppc_recomp.235.cpp", "sub_82AE4280", "pkt=%08X fd=%d",
     ["ctx.r3.u32", "(int)GR(ctx.r3.u32+20)"]),
    ("ppc_recomp.238.cpp", "sub_82AFDA90", "r3=%08X r4=%08X r5=%08X r6=%llX lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "ctx.r5.u32", "(unsigned long long)ctx.r6.u64",
      "(uint32_t)ctx.lr"]),
    ("ppc_recomp.238.cpp", "sub_82AFDC90", "r3=%08X r4=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.268.cpp", "sub_82BF6D30", "r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "ctx.r5.u32", "ctx.r6.u32", "ctx.r7.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.268.cpp", "sub_82BFBE08", "handle=%08X out=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.234.cpp", "sub_82AE2880", "svc=%08X pkt=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.213.cpp", "sub_82A68428", "alloc size=%u tag=%d lr=%08X",
     ["ctx.r4.u32", "(int)ctx.r6.s32", "(uint32_t)ctx.lr"],
     "(int)ctx.r6.s32 == -249"),
    # --- TitleMode park discriminator (P1): which state the Namco-logo screen wrapper is in
    #     (0x823D7638=waiting flags, 0x823D7630=dead error, 0x823D7968=never ticked, 0x823D7628=ready)
    ("ppc_recomp.117.cpp", "sub_82625910", "impl=%08X w=%08X wstate=%08X",
     ["ctx.r3.u32", "GR(ctx.r3.u32+184)", "GR(GR(ctx.r3.u32+184)+40)"]),
    # --- TitleMode state_wait (installed after the logo screen goes ready) ---
    ("ppc_recomp.117.cpp", "sub_826255E0", "impl=%08X lr=%08X",
     ["ctx.r3.u32", "(uint32_t)ctx.lr"]),
    # --- P4: loader error delivery + completion callback ---
    ("ppc_recomp.219.cpp", "sub_82A86180", "screen=%08X status=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    # --- save/content check chain (boot stage-3 gate producer) ---
    ("ppc_recomp.136.cpp", "sub_826B7CC8", "this=%08X lr=%08X",
     ["ctx.r3.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.168.cpp", "sub_82853898", "r3=%08X r4=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.168.cpp", "sub_82854CD8", "r3=%08X r4=%08X r5=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "ctx.r5.u32", "(uint32_t)ctx.lr"]),
    # --- sign-in record refresh (should fire on the initial 0xA broadcast) ---
    ("ppc_recomp.125.cpp", "sub_82667568", "r3=%08X r4=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    # --- boot object init (writes [0x82E6B558+0], the pump head gate) ---
    ("ppc_recomp.135.cpp", "sub_826B3778", "r3=%08X r4=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    # --- boot op#1 (storage/profile 18-state sequence) + dispatcher gate pump ---
    ("ppc_recomp.135.cpp", "sub_826B2708", "w=%08X pumpstate=%u",
     ["ctx.r3.u32", "GR(ctx.r3.u32+32)"]),
    ("ppc_recomp.246.cpp", "sub_82B396D8", "op=%08X state=%u status=%08X",
     ["ctx.r3.u32", "GR(ctx.r3.u32+4)", "GR(ctx.r3.u32+8)"]),
    # --- main-loop iteration timing (rdtsc/3e6 ~ ms on a ~3GHz host) ---
    ("ppc_recomp.94.cpp", "sub_82583288", "t=%u",
     ["(unsigned)(__rdtsc() / 3000000ull)"]),
    ("ppc_recomp.132.cpp", "sub_8269AEF0", "t=%u r3=%08X",
     ["(unsigned)(__rdtsc() / 3000000ull)", "ctx.r3.u32"]),
    ("ppc_recomp.126.cpp", "sub_8266D8C0", "t=%u",
     ["(unsigned)(__rdtsc() / 3000000ull)"]),
    ("ppc_recomp.156.cpp", "sub_827413B0", "t=%u",
     ["(unsigned)(__rdtsc() / 3000000ull)"]),
    # --- flow-arm chain: boot object state -> async poll -> push -> arm ---
    ("ppc_recomp.136.cpp", "sub_826BA2C8", "r3=%08X st=%08X lr=%08X",
     ["ctx.r3.u32", "GR(ctx.r3.u32+4)", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.135.cpp", "sub_826B5420", "r3=%08X lr=%08X",
     ["ctx.r3.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.135.cpp", "sub_826B2B60", "r3=%08X r4=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.126.cpp", "sub_8266CD08", "mgr=%08X r4=%08X r5=%08X r6=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "ctx.r5.u32", "ctx.r6.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.144.cpp", "sub_826F21C0", "r3=%08X r4=%08X r5=%08X r6=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "ctx.r5.u32", "ctx.r6.u32", "(uint32_t)ctx.lr"]),
    # --- boot check-gate work queue (sub_822659C8 singleton 0x82F2B6DC) ---
    ("ppc_recomp.57.cpp", "sub_82432700", "r3=%08X r4=%08X r5=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "ctx.r5.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.57.cpp", "sub_82432AB8", "r3=%08X r4=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.3.cpp", "sub_822659C8", "lr=%08X", ["(uint32_t)ctx.lr"]),
    # --- D3D present/submit chain: find where the device pointer becomes 0 ---
    ("ppc_recomp.156.cpp", "sub_827413B0", "r3=%08X r4=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.155.cpp", "sub_8273A180", "r3=%08X r4=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.155.cpp", "sub_82739E40", "r3=%08X r4=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.155.cpp", "sub_827396F0", "r3=%08X r4=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.155.cpp", "sub_82739C40", "r3=%08X r4=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.155.cpp", "sub_82738B70", "r3=%08X r4=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.155.cpp", "sub_82739210", "r3=%08X r4=%08X r5=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "ctx.r5.u32", "(uint32_t)ctx.lr"]),
    # --- D3D ring-space wait (main-thread spin site) ---
    ("ppc_recomp.155.cpp", "sub_82738830", "dev=%08X wptr=%X n=%X P60=%X mir=%X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "ctx.r5.u32", "GR(GR(ctx.r3.u32+11024)+60)",
      "GR(ctx.r3.u32+11088)", "(uint32_t)ctx.lr"]),
    # --- resc name table / TOC parse ---
    ("ppc_recomp.218.cpp", "sub_82A83920", "hash=%08X tbl: %08X %08X %08X %08X %08X %08X %08X %08X",
     ["GR(ctx.r4.u32+0)", "GR(ctx.r3.u32+0)", "GR(ctx.r3.u32+4)", "GR(ctx.r3.u32+8)",
      "GR(ctx.r3.u32+12)", "GR(ctx.r3.u32+16)", "GR(ctx.r3.u32+20)", "GR(ctx.r3.u32+24)",
      "GR(ctx.r3.u32+28)"]),
    ("ppc_recomp.228.cpp", "sub_82AB42E0", "dev=%08X r4=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.228.cpp", "sub_82AB29F0", "r3=%08X err=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.219.cpp", "sub_82A84988", "r3=%08X r4=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.218.cpp", "sub_82A80E38", "r3=%08X r4=%08X r5=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "ctx.r5.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.218.cpp", "sub_82A80FB8", "r3=%08X r4=%08X r5=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "ctx.r5.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.218.cpp", "sub_82A81008", "r3=%08X r4=%08X r5=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "ctx.r5.u32", "(uint32_t)ctx.lr"]),
    ("ppc_recomp.218.cpp", "sub_82A82160", "r3=%08X r4=%08X r5=%08X lr=%08X",
     ["ctx.r3.u32", "ctx.r4.u32", "ctx.r5.u32", "(uint32_t)ctx.lr"]),
]

HELPER = ('#include <cstdio>\n'
          '#include <cstdlib>\n'
          '#define GR(a) __builtin_bswap32(*(uint32_t*)(base + (uint32_t)(a)))\n'
          '// [probe] lines print only with TTT2_PROBES=1 (ungated they put ~56k lines in every log).\n'
          'static const bool g_ttt2Probes = [] { const char* e = std::getenv("TTT2_PROBES"); '
          'return e && *e == \'1\'; }();\n')
LIMIT = 3000

# Durable generated-code patches (re-applied after every regen, like probes).
# (function, marker, anchor-string, insert-after-anchor)
PATCHES = [
    # sub_82AC5DF0 (sound play-by-id): the registry getter sub_82AC5CC8 returns -1 on a
    # miss and this variant (unlike its siblings, e.g. ppc_recomp.36.cpp 0x82266D..) has
    # NO -1 check before the vtable deref. Hardware never hits it at boot only because
    # its io always finishes the bank load first; skip like the sibling sites do
    # (= retail "sound silently doesn't play"). See PROJECT_STATUS session 6.
    ("sub_82AC5DF0", "[patch] sub_82AC5DF0 miss-guard",
     "\tctx.lr = 0x82AC5E18;\n\tsub_82AC5CC8(ctx, base);\n",
     "\t// [patch] sub_82AC5DF0 miss-guard: registry miss returns -1; skip the play call.\n"
     "\tif (ctx.r3.u32 == 0xFFFFFFFF) {\n"
     "\t\tctx.r1.s64 = ctx.r1.s64 + 112;\n"
     "\t\tctx.lr = PPC_LOAD_U32(ctx.r1.u32 + -8);\n"
     "\t\tctx.f31.u64 = PPC_LOAD_U64(ctx.r1.u32 + -32);\n"
     "\t\tctx.r30.u64 = PPC_LOAD_U64(ctx.r1.u32 + -24);\n"
     "\t\tctx.r31.u64 = PPC_LOAD_U64(ctx.r1.u32 + -16);\n"
     "\t\treturn;\n"
     "\t}\n"),
]

# Decoder calls have volatile argument registers, so capture them around the
# actual generated call, before subsequent guest instructions overwrite r3.
# The audio and video workers share their error handler but use different queues.
VIDEO_ERROR_CALLS = [("8277E114", "audio", "sub_8277DFD8"),
                     ("8277E420", "video", "sub_8277E2B8")]


def add_video_error_probes(text):
    for return_pc, stream, function in VIDEO_ERROR_CALLS:
        if f"PPC_FUNC_IMPL(__imp__{function})" not in text:
            continue
        marker = f"[video-error] {stream} return={return_pc}"
        if marker in text:
            continue
        anchor = (f"\tctx.lr = 0x{return_pc};\n"
                  "\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n")
        if text.count(anchor) != 1:
            raise ValueError(f"decoder call anchor missing or ambiguous: {return_pc}")
        replacement = (
            f"\tctx.lr = 0x{return_pc};\n"
            "\t{\n"
            "\t\tconst uint32_t videoTraceDecoder = ctx.r3.u32;\n"
            "\t\tconst uint32_t videoTraceBytes = ctx.r4.u32;\n"
            "\t\tconst uint32_t videoTraceOut = ctx.r5.u32;\n"
            "\t\tconst uint32_t videoTraceFunction = ctx.ctr.u32;\n"
            "\t\tPPC_CALL_INDIRECT_FUNC(ctx.ctr.u32);\n"
            "\t\tstatic const bool videoTraceEnabled = std::getenv(\"TTT2_VIDEO_TRACE\") != nullptr;\n"
            "\t\tstatic std::atomic<unsigned> videoTraceCount{0};\n"
            "\t\tif (videoTraceEnabled && ctx.r3.s32 < 0 && videoTraceCount.fetch_add(1) < 32) {\n"
            "\t\t\tconst uint32_t videoTraceQueue = PPC_LOAD_U32(ctx.r1.u32 + 84);\n"
            f'\t\t\tfprintf(stderr, "{marker} status=%08X signed=%d fn=%08X decoder=%08X bytes=%u out=%08X written=%u buffer=%08X player=%08X source=%08X queue=%08X count=%u flags=%08X/%08X\\n",\n'
            "\t\t\t\tctx.r3.u32, ctx.r3.s32, videoTraceFunction, videoTraceDecoder,\n"
            "\t\t\t\tvideoTraceBytes, videoTraceOut, PPC_LOAD_U32(videoTraceOut), ctx.r31.u32,\n"
            "\t\t\t\tctx.r30.u32, PPC_LOAD_U32(ctx.r30.u32 + 44), videoTraceQueue,\n"
            "\t\t\t\tPPC_LOAD_U32(videoTraceQueue + 68), PPC_LOAD_U32(ctx.r30.u32 + 224),\n"
            "\t\t\t\tPPC_LOAD_U32(ctx.r30.u32 + 228));\n"
            "\t\t}\n"
            "\t}\n")
        text = text.replace(anchor, replacement, 1)
    for include in ("<cstdio>", "<cstdlib>", "<atomic>"):
        directive = f"#include {include}"
        if directive not in text:
            text = text.replace('#include "ppc_recomp_shared.h"',
                                '#include "ppc_recomp_shared.h"\n' + directive, 1)
    return text


def add_video_raw_probe(text):
    marker = "[video-raw] return=827724D0"
    if marker in text:
        return text
    anchor = "\tctx.lr = 0x827724D0;\n\tsub_82779778(ctx, base);\n"
    begin = text.index("PPC_FUNC_IMPL(__imp__sub_82772360)")
    end = text.index("\n}", begin)
    body = text[begin:end]
    if body.count(anchor) != 1:
        raise ValueError("video parser return anchor missing or ambiguous")
    probe = (
        "\t{\n"
        "\t\tstatic const bool videoRawEnabled = std::getenv(\"TTT2_VIDEO_TRACE\") != nullptr;\n"
        "\t\tstatic std::atomic<unsigned> videoRawCount{0};\n"
        "\t\tif (videoRawEnabled && ctx.r3.u32 != 0 && videoRawCount.fetch_add(1) < 64)\n"
        f'\t\t\tfprintf(stderr, "{marker} raw=%08X signed=%d decoder=%08X parser=%08X left=%u pending=%u eof=%u stream=%u\\n",\n'
        "\t\t\t\tctx.r3.u32, ctx.r3.s32, ctx.r30.u32, PPC_LOAD_U32(ctx.r30.u32 + 520),\n"
        "\t\t\t\tctx.r26.u32, PPC_LOAD_U32(ctx.r30.u32 + 540),\n"
        "\t\t\t\tPPC_LOAD_U32(ctx.r30.u32 + 544), PPC_LOAD_U32(ctx.r31.u32 + 96));\n"
        "\t}\n")
    text = text[:begin] + body.replace(anchor, anchor + probe, 1) + text[end:]
    for include in ("<cstdio>", "<cstdlib>", "<atomic>"):
        directive = f"#include {include}"
        if directive not in text:
            text = text.replace('#include "ppc_recomp_shared.h"',
                                '#include "ppc_recomp_shared.h"\n' + directive, 1)
    return text


def add_video_inner_probe(text):
    marker = "[video-inner] return=827797E8"
    if marker in text:
        return text
    begin = text.index("PPC_FUNC_IMPL(__imp__sub_82779778)")
    end = text.index("\n}", begin)
    body = text[begin:end]
    anchor = "\tctx.lr = 0x827797E8;\n\tsub_827770E0(ctx, base);\n"
    if body.count(anchor) != 1:
        raise ValueError("inner video parser return anchor missing or ambiguous")
    probe = (
        "\t{\n"
        "\t\tstatic const bool videoInnerEnabled = std::getenv(\"TTT2_VIDEO_TRACE\") != nullptr;\n"
        "\t\tstatic std::atomic<unsigned> videoInnerCount{0};\n"
        "\t\tif (videoInnerEnabled && ctx.r3.u32 != 0 && videoInnerCount.fetch_add(1) < 64)\n"
        f'\t\t\tfprintf(stderr, "{marker} hresult=%08X signed=%d parser=%08X p0=%08X p4=%08X p528=%08X p532=%08X p568=%08X p572=%08X p608=%08X p632=%08X\\n",\n'
        "\t\t\t\tctx.r3.u32, ctx.r3.s32, ctx.r31.u32,\n"
        "\t\t\t\tPPC_LOAD_U32(ctx.r31.u32), PPC_LOAD_U32(ctx.r31.u32 + 4),\n"
        "\t\t\t\tPPC_LOAD_U32(ctx.r31.u32 + 528), PPC_LOAD_U32(ctx.r31.u32 + 532),\n"
        "\t\t\t\tPPC_LOAD_U32(ctx.r31.u32 + 568), PPC_LOAD_U32(ctx.r31.u32 + 572),\n"
        "\t\t\t\tPPC_LOAD_U32(ctx.r31.u32 + 608), PPC_LOAD_U32(ctx.r31.u32 + 632));\n"
        "\t}\n")
    text = text[:begin] + body.replace(anchor, anchor + probe, 1) + text[end:]
    for include in ("<cstdio>", "<cstdlib>", "<atomic>"):
        directive = f"#include {include}"
        if directive not in text:
            text = text.replace('#include "ppc_recomp_shared.h"',
                                '#include "ppc_recomp_shared.h"\n' + directive, 1)
    return text


def locate_functions(functions):
    locations = {}
    pattern = re.compile(r"PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]+)\)")
    for path in PPC.glob("ppc_recomp.*.cpp"):
        for function in pattern.findall(path.read_text(encoding="utf-8")):
            if function in functions:
                if function in locations:
                    raise ValueError(f"duplicate generated function: {function}")
                locations[function] = path.name
    missing = functions - locations.keys()
    if missing:
        raise ValueError(f"generated functions missing: {sorted(missing)}")
    return locations


def main():
    functions = ({entry[1] for entry in PROBES} |
                 {entry[0] for entry in PATCHES} |
                 {entry[2] for entry in VIDEO_ERROR_CALLS} |
                 {"sub_82772360", "sub_82779778"})
    locations = locate_functions(functions)
    by_file = {}
    for entry in PROBES:
        fn, fmt, args = entry[1], entry[2], entry[3]
        f = locations[fn]
        cond = entry[4] if len(entry) > 4 else None
        by_file.setdefault(f, []).append((fn, fmt, args, cond))

    for fname, probes in by_file.items():
        path = PPC / fname
        text = path.read_text(encoding="utf-8", errors="replace")
        changed = False
        if "#define GR(a)" not in text:
            text = text.replace('#include "ppc_recomp_shared.h"',
                                '#include "ppc_recomp_shared.h"\n' + HELPER, 1)
            changed = True
        for fn, fmt, args, cond in probes:
            marker = f'[probe] {fn}'
            if marker in text:
                print(f"  {fname}: {fn} already probed")
                continue
            pat = re.compile(
                r'(PPC_FUNC_IMPL\(__imp__' + fn + r'\)\s*\{\n(?:\s*PPC_FUNC_PROLOGUE\(\);\n)?)')
            guard = f'if (({cond})) ' if cond else ''
            probe = (f'\t{{ static int _n; {guard}if (g_ttt2Probes && _n++ < {LIMIT}) '
                     f'fprintf(stderr, "[probe] {fn} {fmt}\\n", '
                     + ", ".join(args) + '); }\n')
            # lambda replacement: keeps backslashes in `probe` literal (subn would eat \n)
            text, n = pat.subn(lambda m: m.group(1) + probe, text, count=1)
            if n != 1:
                print(f"  ERROR: {fname}: {fn} pattern not found", file=sys.stderr)
                sys.exit(1)
            changed = True
            print(f"  {fname}: probed {fn}")
        if changed:
            path.write_text(text, encoding="utf-8")

    for function, marker, anchor, insert in PATCHES:
        fname = locations[function]
        path = PPC / fname
        text = path.read_text(encoding="utf-8", errors="replace")
        if marker in text:
            print(f"  {fname}: patch already applied ({marker})")
            continue
        if anchor not in text:
            print(f"  ERROR: {fname}: patch anchor not found ({marker})", file=sys.stderr)
            sys.exit(1)
        text = text.replace(anchor, anchor + insert, 1)
        path.write_text(text, encoding="utf-8")
        print(f"  {fname}: patched ({marker})")

    for fname in sorted({locations[entry[2]] for entry in VIDEO_ERROR_CALLS}):
        video_path = PPC / fname
        before = video_path.read_text(encoding="utf-8")
        after = add_video_error_probes(before)
        if before != after:
            video_path.write_text(after, encoding="utf-8")
            print(f"  {fname}: added opt-in decoder error probes")
    raw_path = PPC / locations["sub_82772360"]
    before = raw_path.read_text(encoding="utf-8")
    after = add_video_raw_probe(before)
    if before != after:
        raw_path.write_text(after, encoding="utf-8")
        print(f"  {raw_path.name}: added opt-in raw video parser status probe")
    inner_path = PPC / locations["sub_82779778"]
    before = inner_path.read_text(encoding="utf-8")
    after = add_video_inner_probe(before)
    if before != after:
        inner_path.write_text(after, encoding="utf-8")
        print(f"  {inner_path.name}: added opt-in inner video parser HRESULT probe")
    print("done")


if __name__ == "__main__":
    main()
