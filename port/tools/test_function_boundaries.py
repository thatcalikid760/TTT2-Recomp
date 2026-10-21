"""Regression for conditional branch function boundaries.

Synthetic branch tests need only the built XenonUtils/disasm libraries. Add
--check-local-game to analyze the local memset image and execute its generated
body plus four branch-fallthrough helpers; game instructions are read locally into temporary
files and never embedded in this test. --negative-control restores the old BO
predicate in a temporary analyzer copy and requires a failure. Use
--negative-projection-control to restore the old zero-length fallthrough block.
"""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from test_vcmpgtuh import compile_one


ROOT = Path(__file__).resolve().parents[2]
RECOMP = ROOT / "tools/XenonRecomp"

ANALYZER_TEST = r'''
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>
#include "@ANALYZER@"
int main(int argc,char** argv) {
    int failures=0;
    for(unsigned bo:{4u,12u,16u,18u,20u}) {
        uint32_t code[]={__builtin_bswap32((19u<<26)|(bo<<21)|(16u<<1)),
            __builtin_bswap32(0x60000000u),__builtin_bswap32(0x4E800020u),0,0};
        auto fn=Function::Analyze(code,sizeof(code),0x82000000u);
        size_t expected=bo==20?4:12;
        std::printf("bclr BO=%u analyzed=%zu expected=%zu\n",bo,fn.size,expected);
        failures+=fn.size!=expected;
    }
    for(unsigned bo:{4u,12u,16u,18u,20u}) {
        // Both outcomes reach the same next instruction. BO does not change that.
        uint32_t code[]={__builtin_bswap32((16u<<26)|(bo<<21)|(2u<<16)|4u),
            __builtin_bswap32(0x3860000Fu),__builtin_bswap32(0x4E800020u),0,0};
        auto fn=Function::Analyze(code,sizeof(code),0x82000000u);
        std::printf("bc-next BO=%u analyzed=%zu expected=12\n",bo,fn.size);
        failures+=fn.size!=12;
    }
    for(int displacement:{-4,8}) {
        uint32_t code[]={__builtin_bswap32(0x60000000u),
            __builtin_bswap32((16u<<26)|(4u<<21)|(2u<<16)|(uint32_t(displacement)&0xFFFC)),
            __builtin_bswap32(0x3860000Fu),__builtin_bswap32(0x4E800020u),0,0};
        auto fn=Function::Analyze(code,sizeof(code),0x82000000u);
        std::printf("bc displacement=%d analyzed=%zu expected=16\n",displacement,fn.size);
        failures+=fn.size!=16;
    }
    if(argc>1) {
        std::ifstream image(argv[1],std::ios::binary);
        std::vector<uint32_t> code(0x200/4);
        image.seekg(0xBD4320);
        if(!image.read(reinterpret_cast<char*>(code.data()),0x200)) return 2;
        auto fn=Function::Analyze(code.data(),0x200,0x82BD4320u);
        std::printf("local memset analyzed=0x%zX expected=0xA0\n",fn.size);
        failures+=fn.size!=0xA0;
    }
    std::printf("%s analyzer boundaries (%d failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
'''

MEMSET_TEST = r'''
#include "ppc_config.h"
#include "ppc_context.h"
#include <array>
#include <cstdio>
#include <cstring>
static void Reporter(PPCContext&,uint8_t*) {}
PPCFunc* const g_unmappedReporter=Reporter;
@BODY@
int main() {
    unsigned failures=0,checks=0;
    alignas(32) std::array<uint8_t,256> actual,expected;
    for(unsigned offset=0;offset<16;offset++) for(unsigned count=0;count<=128;count++)
    for(unsigned byte:{0u,0xA5u,0xFFu}) {
        actual.fill(0x37);expected=actual;
        std::memset(expected.data()+32+offset,byte,count);
        PPCContext ctx{};ctx.r3.u64=32+offset;ctx.r4.u64=byte;ctx.r5.u64=count;
        __imp__sub_82BD4320(ctx,actual.data());++checks;
        if(actual!=expected || ctx.r3.u32!=32+offset) {
            if(failures<3) std::printf("mismatch offset=%u count=%u byte=%02X\n",offset,count,byte);
            failures++;
        }
    }
    std::printf("%s memset cases=%u failures=%u\n",failures?"FAIL":"PASS",checks,failures);
    return failures?1:0;
}
'''

FALLTHROUGH_TEST = r'''
#include "ppc_config.h"
#include "ppc_context.h"
#include <array>
#include <cstdio>
static void Reporter(PPCContext&,uint8_t*) {}
PPCFunc* const g_unmappedReporter=Reporter;
@BODY@
int main() {
    alignas(32) std::array<uint8_t,1024> memory{};
    uint8_t* base=memory.data(); PPCContext ctx{}; unsigned failures=0;
    ctx.r3.u32=0x80600000; __imp__sub_82778048(ctx,base);
    std::printf("status mapping actual=%08X expected=0000000F\n",ctx.r3.u32);
    failures+=ctx.r3.u32!=15;
    ctx={};ctx.r3.u32=64;ctx.r4.u32=0;__imp__sub_82628FD8(ctx,base);
    std::printf("zero selector actual=%u expected=128\n",ctx.r3.u32);
    failures+=ctx.r3.u32!=128;
    PPC_STORE_U32(64+72,256);
    ctx={};ctx.r3.u32=64;ctx.r4.u32=1;ctx.r5.u32=0;__imp__sub_82629030(ctx,base);
    std::printf("array zero selector actual=%u expected=396\n",ctx.r3.u32);
    failures+=ctx.r3.u32!=396;
    PPC_STORE_U8(64+37,1);PPC_STORE_U64(64+24,1000000);
    ctx={};ctx.r3.u32=64;__imp__sub_8264BE80(ctx,base);
    std::printf("threshold classification actual=%u expected=7\n",ctx.r3.u32);
    failures+=ctx.r3.u32!=7;
    std::printf("%s local conditional fallthrough helpers (%u failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
'''


def local_fallthrough_bodies(generated):
    """Read local generated functions and their direct helper dependencies only."""
    functions = {}
    pattern = re.compile(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\) \{.*?\n\}", re.S)
    for path in generated.glob("ppc_recomp.*.cpp"):
        for match in pattern.finditer(path.read_text(encoding="utf-8")):
            functions[match[1]] = match[0]
    names = {"82778048", "82628FD8", "82629030", "8264BE80"}
    pending = list(names)
    while pending:
        body = functions[pending.pop()]
        for dependency in re.findall(r"\bsub_([0-9A-F]+)\(ctx, base\)", body):
            if dependency not in names:
                names.add(dependency)
                pending.append(dependency)
    declarations = "\n".join(f'extern "C" PPC_FUNC(__imp__sub_{name});' for name in sorted(names))
    bodies = "\n".join(re.sub(r"\bsub_([0-9A-F]+)\(ctx, base\)",
                               r"__imp__sub_\1(ctx, base)", functions[name]) for name in sorted(names))
    return declarations + "\n" + bodies


def compile_analyzer(folder, source):
    vcvars = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
    clang = Path(r"C:\Program Files\LLVM\bin\clang-cl.exe")
    includes = [RECOMP / "XenonAnalyse", RECOMP / "XenonUtils", RECOMP / "thirdparty/disasm"]
    libraries = [RECOMP / "build/XenonUtils/XenonUtils.lib", RECOMP / "build/thirdparty/disasm/disasm.lib"]
    for library in libraries:
        if not library.exists():
            raise SystemExit(f"Build tools/build_xenonrecomp.cmd first; missing {library}")
    cpp, exe = folder / "boundary.cpp", folder / "boundary.exe"
    cpp.write_text(source, encoding="utf-8")
    include_args = " ".join(f'/I"{path}"' for path in includes)
    library_args = " ".join(f'"{path}"' for path in libraries)
    batch = folder / "boundary.cmd"
    batch.write_text(
        f'@echo off\ncall "{vcvars}" >nul\nif errorlevel 1 exit /b 1\n'
        f'"{clang}" /nologo /std:c++17 /O2 /EHsc /MT {include_args} "{cpp}" '
        f'/Fe:"{exe}" /Fo:"{folder / "boundary.obj"}" /link {library_args}\n', encoding="utf-8")
    subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=folder, check=True)
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-local-game", action="store_true")
    parser.add_argument("--negative-control", action="store_true")
    parser.add_argument("--negative-projection-control", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ttt2-boundaries-") as temp:
        folder = Path(temp)
        analyzer = RECOMP / "XenonAnalyse/function.cpp"
        if args.negative_control:
            source = analyzer.read_text(encoding="utf-8")
            fixed = "(PPC_BO(instruction) & 0x14) != 0x14"
            if fixed not in source:
                raise SystemExit("Conditional branch predicate not found")
            analyzer = folder / "old_function.cpp"
            analyzer.write_text(source.replace(fixed, "!(PPC_BO(instruction) & 0x10)"), encoding="utf-8")
        if args.negative_projection_control:
            source = analyzer.read_text(encoding="utf-8")
            fixed = "rBase > lBase ? rBase - lBase : size_t(-1)"
            if fixed not in source:
                raise SystemExit("Conditional fallthrough projection not found")
            analyzer = folder / "old_projection.cpp"
            analyzer.write_text(source.replace(fixed, "rBase - lBase"), encoding="utf-8")
        exe = compile_analyzer(folder, ANALYZER_TEST.replace("@ANALYZER@", analyzer.as_posix()))
        command = [str(exe)]
        if args.check_local_game:
            command.append(str(ROOT / "re/decrypted/Default_patched_tu3.bin"))
        result = subprocess.run(command)
        if args.negative_control or args.negative_projection_control:
            if result.returncode != 1:
                raise SystemExit("Negative control did not produce the expected regression failure")
            print("PASS negative control: conditional fallthrough truncation detected")
            return
        result.check_returncode()
        if args.check_local_game:
            generated = ROOT / "re/ppc"
            pattern = re.compile(r"PPC_FUNC_IMPL\(__imp__sub_82BD4320\) \{.*?\n\}", re.S)
            body = None
            for path in generated.glob("ppc_recomp.*.cpp"):
                match = pattern.search(path.read_text(encoding="utf-8"))
                if match:
                    body = match.group()
                    break
            if body is None:
                raise SystemExit("Regenerate the local PPC sources before --check-local-game")
            checker = compile_one(folder, "memset", MEMSET_TEST.replace("@BODY@", body),
                                  [generated, RECOMP / "thirdparty/simde"])
            subprocess.run([str(checker)], check=True)
            checker = compile_one(folder, "fallthrough", FALLTHROUGH_TEST.replace(
                "@BODY@", local_fallthrough_bodies(generated)), [generated, RECOMP / "thirdparty/simde"])
            subprocess.run([str(checker)], check=True)


if __name__ == "__main__":
    main()
