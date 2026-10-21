"""VMX128 half-float pack/unpack regression: the code XenonRecomp now emits for
vpkd3d128 type 3 (FLOAT16_2) and vupkd3d128 types 3/5 (FLOAT16_2 / FLOAT16_4), which used to
be `__builtin_debugtrap()`. The trap in sub_827526E0 (vpkd3d128 v63,v127,3,1,3) killed a render
job at the end of every match and froze the game.

Compiles the blocks exactly as they appear in re/ppc (first site of each form) against
re/ppc/ppc_context.h and checks them on edge values against an independent reference:
  unpack FLOAT16_4: PPC halfwords 4..7 -> words 0..3 (layout of TTT2's half->float stream
                    routine, which unpacks the vector's low 8 bytes first)
  unpack FLOAT16_2: PPC halfwords 6, 7 -> words 0, 1; words 2, 3 = 0, 1
  pack FLOAT16_2, VPACK_32 shift 3: word 0 = half(x) << 16 | half(y); other words kept
Xenos halves: exponent 31 is a normal exponent (no inf/NaN), packing truncates, saturates to
0x7FFF, and flushes below the half denormal range to zero.
Negative control (--negative-control): swap the unpack layout expectation; the test must fail.
"""

import argparse
import re
import subprocess
import tempfile
from pathlib import Path

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
PPC = ROOT / "re/ppc"
SIMDE = ROOT / "tools/XenonRecomp/thirdparty/simde"


def block(pattern):
    """The emitted lines of the first instruction whose comment matches `pattern`."""
    for path in sorted(PPC.glob("ppc_recomp.*.cpp")):
        lines = path.read_bytes().decode("utf-8").split("\n")
        for i, line in enumerate(lines):
            if re.match(r"\s*// " + pattern + r"\s*$", line.rstrip("\r")):
                out, j = [], i + 1
                while j < len(lines) and not re.match(r"\s*// [a-z]", lines[j]) and not re.match(r"loc_[0-9A-F]+:", lines[j]):
                    out.append(lines[j].rstrip("\r"))
                    j += 1
                return line.strip()[3:], "\n".join(out)
    raise SystemExit(f"no instruction matching {pattern!r} in re/ppc")


HARNESS = r'''
#include "ppc_context.h"
#include <cmath>
#include <cstdio>
#include <cstring>

static unsigned checks = 0, failures = 0;
static void Check(bool ok, const char* what, unsigned a, unsigned b)
{
    if (ok) ++checks; else { ++failures; if (failures < 20) std::fprintf(stderr, "FAIL %s: got %08X want %08X\n", what, a, b); }
}
static uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

// Reference Xenos half -> float.
static float HalfToFloat(uint16_t h)
{
    const int e = (h >> 10) & 31, m = h & 1023;
    const double v = e ? std::ldexp(1.0 + m / 1024.0, e - 15) : std::ldexp(double(m), -24);
    return float((h & 0x8000) ? -v : v);
}
// Reference float -> Xenos half (truncating, saturating, denormals below 2^-14).
static uint16_t FloatToHalf(float f)
{
    const uint16_t sign = (Bits(f) >> 16) & 0x8000;
    const double a = std::fabs(double(f));
    if (f != f || a > 65504.0) return uint16_t(0x7FFF | sign);
    if (a < std::ldexp(1.0, -14)) return uint16_t(uint32_t(std::floor(a * 16777216.0)) | sign);
    int e; const double fr = std::frexp(a, &e);          // a = fr * 2^e, fr in [0.5, 1)
    const int m = int(std::floor((fr * 2.0 - 1.0) * 1024.0));
    return uint16_t((((e - 1 + 15) << 10) | m) | sign);
}

// PPC element k of a vector lives in host lane 3 - k (u32/f32) or 7 - k (u16).
static void Unpack4(PPCContext& __restrict ctx, uint8_t* base) { PPCRegister temp{}; PPCVRegister vTemp{}; uint32_t ea{}; (void)ea; (void)base;
@UNPACK4@
}
static void Unpack2(PPCContext& __restrict ctx, uint8_t* base) { PPCRegister temp{}; PPCVRegister vTemp{}; uint32_t ea{}; (void)ea; (void)base;
@UNPACK2@
}
static void Pack2(PPCContext& __restrict ctx, uint8_t* base) { PPCRegister temp{}; PPCVRegister vTemp{}; uint32_t ea{}; (void)ea; (void)base;
@PACK2@
}

int main()
{
    static const uint16_t halves[] = { 0x0000, 0x8000, 0x0001, 0x8001, 0x03FF, 0x0400, 0x3C00, 0xBC00, 0x3555,
                                       0x7BFF, 0x7C00, 0x7FFF, 0xFFFF, 0x5640, 0x1234, 0xC000, 0x0200, 0x7E00 };
    const int n = int(sizeof(halves) / sizeof(halves[0]));
    alignas(16) static uint8_t mem[64];
    for (int r = 0; r < n; ++r)
    {
        PPCContext ctx{};
        uint16_t h[8];
        for (int k = 0; k < 8; ++k) h[k] = halves[(r + 3 * k) % n];
        for (int k = 0; k < 8; ++k) ctx.@U4SRC@.u16[7 - k] = h[k];
        Unpack4(ctx, mem);
        for (int k = 0; k < 4; ++k)
            Check(ctx.@U4DST@.u32[3 - k] == Bits(HalfToFloat(h[@U4FIRST@ + k])), "unpack float16_4", ctx.@U4DST@.u32[3 - k], Bits(HalfToFloat(h[@U4FIRST@ + k])));
        PPCContext c2{};
        for (int k = 0; k < 8; ++k) c2.@U2SRC@.u16[7 - k] = h[k];
        Unpack2(c2, mem);
        Check(c2.@U2DST@.u32[3] == Bits(HalfToFloat(h[6])), "unpack float16_2 x", c2.@U2DST@.u32[3], Bits(HalfToFloat(h[6])));
        Check(c2.@U2DST@.u32[2] == Bits(HalfToFloat(h[7])), "unpack float16_2 y", c2.@U2DST@.u32[2], Bits(HalfToFloat(h[7])));
        Check(c2.@U2DST@.f32[1] == 0.0f && c2.@U2DST@.f32[0] == 1.0f, "unpack float16_2 z/w", c2.@U2DST@.u32[1], 0);
    }
    static const float floats[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.333333f, 65504.0f, 65520.0f, -70000.0f, 1e-5f, -6e-8f,
                                    1e-9f, 3.14159f, 1024.75f, 6.1e-5f, 3.0e-5f, 2.0f, -0.5f, 123.456f };
    const int nf = int(sizeof(floats) / sizeof(floats[0]));
    for (int r = 0; r < nf; ++r)
    {
        PPCContext ctx{};
        const float x = floats[r], y = floats[(r + 5) % nf];
        ctx.@PSRC@.f32[3] = x; ctx.@PSRC@.f32[2] = y; ctx.@PSRC@.f32[1] = 7.0f; ctx.@PSRC@.f32[0] = 9.0f;
        for (int k = 0; k < 4; ++k) ctx.@PDST@.u32[k] = 0xA5A50000u + k;
        Pack2(ctx, mem);
        const uint32_t want = (uint32_t(FloatToHalf(x)) << 16) | FloatToHalf(y);
        Check(ctx.@PDST@.u32[@PWORD@] == want, "pack float16_2", ctx.@PDST@.u32[@PWORD@], want);
        for (int k = 0; k < 4; ++k)
            if (k != @PWORD@) Check(ctx.@PDST@.u32[k] == 0xA5A50000u + k, "pack keeps other words", ctx.@PDST@.u32[k], 0xA5A50000u + k);
    }
    if (failures) { std::fprintf(stderr, "%u of %u checks failed\n", failures, checks + failures); return 1; }
    std::printf("PASS %u half pack/unpack checks (vupkd3d128 float16_4/float16_2, vpkd3d128 float16_2)\n", checks);
    return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--negative-control", action="store_true")
    args = ap.parse_args()
    u4_text, u4 = block(r"vupkd3d128 v\d+,v\d+,20")
    u2_text, u2 = block(r"vupkd3d128 v\d+,v\d+,12")
    p2_text, p2 = block(r"vpkd3d128 v\d+,v\d+,3,1,\d")
    for text, code in ((u4_text, u4), (u2_text, u2), (p2_text, p2)):
        if "debugtrap" in code:
            raise SystemExit(f"FAIL {text} is still a trap in re/ppc")
    def regs(text):
        return text.split()[1].split(",")
    u4d, u4s = regs(u4_text)[:2]
    u2d, u2s = regs(u2_text)[:2]
    pd, ps, _, _, shift = regs(p2_text)
    src = HARNESS
    for key, value in {"@UNPACK4@": u4, "@UNPACK2@": u2, "@PACK2@": p2,
                       "@U4SRC@": "v" + u4s[1:], "@U4DST@": "v" + u4d[1:], "@U2SRC@": "v" + u2s[1:],
                       "@U2DST@": "v" + u2d[1:], "@PSRC@": "v" + ps[1:], "@PDST@": "v" + pd[1:],
                       "@PWORD@": shift, "@U4FIRST@": "0" if args.negative_control else "4"}.items():
        src = src.replace(key, value)
    print(f"blocks: {u4_text} | {u2_text} | {p2_text}")
    with tempfile.TemporaryDirectory(prefix="ttt2-vpk-") as temp:
        exe = compile_one(Path(temp), "vpk", src, [PPC, SIMDE])
        result = subprocess.run([str(exe)], text=True, capture_output=True)
    print(result.stdout + result.stderr, end="")
    if args.negative_control:
        if result.returncode == 0:
            raise SystemExit("Negative control unexpectedly passed")
        print("PASS negative control: a high-half unpack layout is detected")
    elif result.returncode:
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
