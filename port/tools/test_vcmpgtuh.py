"""Standalone VCMPGTUH emitter/CR6 regression; no game or full regen required.

Extracts the production C++ switch case, compiles a tiny emitter, and executes
its output with the real PPC vector/CR helpers. Scalar unsigned comparisons
provide the oracle. Reference semantics: Xenia ppc_emit_altivec.cc
InstrEmit_vcmpxxi_ and ppc_hir_builder.cc UpdateCR6 (all/none, bits 1/3 zero).
"""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
RECOMP = ROOT / "tools/XenonRecomp"

EMITTER = r'''
#include <cstring>
#include <cstdio>
#include <string>
#define FMT_HEADER_ONLY
#include <fmt/format.h>
std::string output;
template<class... Args> void println(fmt::format_string<Args...> pattern, Args&&... args) {
    output += fmt::format(pattern, std::forward<Args>(args)...) + "\n";
}
std::string v(int i) { return fmt::format("vr[{}]", i); }
std::string cr(int i) { return fmt::format("cr{}", i); }
struct Opcode { const char* name; };
struct Instruction { Opcode* opcode; int operands[3]; };
constexpr int PPC_INST_VCMPGTUH = 0;
void emit(Instruction insn) {
    switch (0) {
@CASE@
    }
}
int main() {
    for (int record=0; record<2; ++record) {
        for (int alias=0; alias<4; ++alias) {
            Opcode op{record ? "vcmpgtuh." : "vcmpgtuh"};
            Instruction insn{&op, {alias == 3 ? 0 : alias, 1, alias == 3 ? 1 : 2}};
            output.clear(); emit(insn);
            std::printf("void op%d_%d(PPCVRegister* vr, PPCCRRegister& cr6) {\n%s}\n",
                record, alias, output.c_str());
        }
    }
}
'''

CHECKER = r'''
#define PPC_CONFIG_H_INCLUDED
#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0x11C0000ull
#define PPC_CODE_BASE 0x82250000ull
#define PPC_CODE_SIZE 0xB6848Cull
#include <ppc_context.h>
#include <cstdio>
#include <cstring>
@GENERATED@
using Fn = void(*)(PPCVRegister*, PPCCRRegister&);
Fn functions[2][4] = {{op0_0,op0_1,op0_2,op0_3},{op1_0,op1_1,op1_2,op1_3}};
unsigned cases = 0;
bool check(const uint16_t* a, const uint16_t* b) {
    for (int record=0; record<2; ++record) for (int alias=0; alias<4; ++alias)
    for (int seed=0; seed<16; ++seed) {
        PPCVRegister vr[3]{};
        std::memcpy(vr[1].u16, a, 16); std::memcpy(vr[2].u16, b, 16);
        PPCVRegister expected[3]; std::memcpy(expected, vr, sizeof(vr));
        int destination=alias==3 ? 0 : alias;
        bool all=true, none=true;
        for (int lane=0; lane<8; ++lane) {
            bool gt = a[lane] > (alias==3 ? a[lane] : b[lane]);
            expected[destination].u16[lane] = gt ? 0xFFFF : 0;
            all &= gt; none &= !gt;
        }
        PPCCRRegister cr6{};
        cr6.lt=seed&1; cr6.gt=(seed>>1)&1; cr6.eq=(seed>>2)&1; cr6.so=(seed>>3)&1;
        PPCCRRegister wanted=cr6;
        if (record) { wanted.lt=all; wanted.gt=0; wanted.eq=none; wanted.so=0; }
        functions[record][alias](vr,cr6); ++cases;
        if (std::memcmp(vr,expected,sizeof(vr)) || std::memcmp(&cr6,&wanted,sizeof(cr6))) {
            std::fprintf(stderr,"FAIL case=%u record=%d alias=%d seed=%d CR6=%u%u%u%u expected=%u%u%u%u\n",
                cases,record,alias,seed,cr6.lt,cr6.gt,cr6.eq,cr6.so,wanted.lt,wanted.gt,wanted.eq,wanted.so);
            return false;
        }
    }
    return true;
}
int main() {
    uint16_t a[8],b[8];
    // Every all/none/mixed lane truth pattern, crossing the signed boundary.
    for (unsigned mask=0;mask<256;++mask) {
        for (int lane=0;lane<8;++lane) {
            bool gt=(mask>>lane)&1; a[lane]=gt?0x8000:0x7FFF; b[lane]=gt?0x7FFF:0x8000;
        }
        if (!check(a,b)) return 1;
    }
    const uint16_t edges[]={0,1,0x7FFF,0x8000,0xFFFE,0xFFFF};
    for (auto av:edges) for(auto bv:edges) {
        for(int lane=0;lane<8;++lane) { a[lane]=av; b[lane]=bv; }
        if(!check(a,b)) return 1;
    }
    uint32_t random=0x12345678;
    for(int sample=0;sample<2048;++sample) {
        for(int lane=0;lane<8;++lane) {
            random^=random<<13;random^=random>>17;random^=random<<5;a[lane]=uint16_t(random);
            random^=random<<13;random^=random>>17;random^=random<<5;b[lane]=uint16_t(random);
        }
        if(!check(a,b)) return 1;
    }
    std::printf("PASS %u scalar-oracle cases: 256 truth masks, unsigned edges, random lanes, all CR6 seeds, Rc=0/1, destination/source aliasing\n",cases);
}
'''


def compile_one(folder, name, text, includes):
    vcvars = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
    clang = Path(r"C:\Program Files\LLVM\bin\clang-cl.exe")
    cpp, exe = folder / (name + ".cpp"), folder / (name + ".exe")
    cpp.write_text(text, encoding="utf-8")
    include_args = " ".join('/I"' + str(path) + '"' for path in includes)
    command = f'@echo off\ncall "{vcvars}" >nul\nif errorlevel 1 exit /b 1\n"{clang}" /nologo /std:c++20 /O2 /EHsc /clang:-march=sandybridge {include_args} "{cpp}" /Fe:"{exe}" /Fo:"{folder / (name + ".obj")}"\n'
    batch = folder / (name + ".cmd")
    batch.write_text(command, encoding="utf-8")
    result = subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=folder, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--negative-control", action="store_true", help="Remove only the emitted CR6 update and require the regression to fail")
    args = parser.parse_args()
    source = (RECOMP / "XenonRecomp/recompiler.cpp").read_text(encoding="utf-8-sig")
    match = re.search(r"    case PPC_INST_VCMPGTUH:\n.*?        break;", source, re.S)
    if not match:
        raise SystemExit("VCMPGTUH emitter case not found")
    with tempfile.TemporaryDirectory(prefix="ttt2-vcmpgtuh-") as temp:
        folder = Path(temp)
        emitter = compile_one(folder, "emit", EMITTER.replace("@CASE@", match.group()), [RECOMP / "thirdparty/fmt/include"])
        generated = subprocess.check_output([str(emitter)], text=True)
        if args.negative_control:
            generated = re.sub(r"^.*setFromMask.*\n", "", generated, flags=re.M)
        checker = compile_one(folder, "check", CHECKER.replace("@GENERATED@", generated), [RECOMP / "XenonUtils", RECOMP / "thirdparty/simde"])
        result = subprocess.run([str(checker)], text=True, capture_output=True)
        print(result.stdout + result.stderr, end="")
        if args.negative_control:
            if result.returncode == 0:
                raise SystemExit("Negative control unexpectedly passed")
            print("PASS negative control: missing CR6 update is detected")
        elif result.returncode:
            raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
