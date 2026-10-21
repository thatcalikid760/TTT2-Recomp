"""Compile the production BCTR emitter and exercise its bounds with both headers.

Valid indices must execute their original cases. Invalid indices must print the
guest branch PC and the full index, then terminate before any case executes.
No game data, regeneration, or full runtime build is needed.
"""
from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
RECOMP = ROOT / "tools/XenonRecomp"

EMITTER = r'''
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#define FMT_HEADER_ONLY
#include <fmt/format.h>
std::string output;
template<class... A> void print(fmt::format_string<A...> f, A&&... a) {
    output += fmt::format(f, std::forward<A>(a)...);
}
template<class... A> void println(fmt::format_string<A...> f, A&&... a) {
    ::print(f, std::forward<A>(a)...); output += "\n";
}
std::string r(unsigned i) { return fmt::format("ctx.r{}", i); }
std::string ctr() { return "ctx.ctr"; }
void printFunctionCall(uint32_t address) { println("sub_{:08X}(ctx, nullptr);", address); }
struct Table { unsigned r; std::vector<uint32_t> labels; };
struct Config { std::map<uint32_t,Table> switchTables; } config;
constexpr unsigned PPC_INST_BCTR = 0;
int main() {
    config.switchTables[0x82ABD354] = {11, {0x82ABD37C,0x82ABD3B4}};
    auto switchTable = config.switchTables.begin();
    struct { uint32_t base, size; } fn{0x82ABD328,0x154};
    uint32_t base = 0x82ABD378;
    switch(PPC_INST_BCTR) {
@CASE@
    }
    std::printf("void dispatch(PPCContext& ctx) {\n%s"
                "loc_82ABD37C: std::puts(\"CASE=0\"); return;\n"
                "loc_82ABD3B4: std::puts(\"CASE=1\"); return;\n}\n", output.c_str());
}
'''

CHECKER = r'''
#define PPC_CONFIG_H_INCLUDED
#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0x11C0000ull
#define PPC_CODE_BASE 0x82250000ull
#define PPC_CODE_SIZE 0xB6848Cull
#include <ppc_context.h>
#include <crtdbg.h>
@GENERATED@
int main(int argc,char** argv) {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    PPCContext ctx{};
    ctx.r11.u64=argc>1?std::strtoull(argv[1],nullptr,16):0;
    dispatch(ctx);
}
'''


def main():
    source = (RECOMP / "XenonRecomp/recompiler.cpp").read_text(encoding="utf-8")
    begin = source.index("    case PPC_INST_BCTR:")
    end = source.index("    case PPC_INST_BCTRL:", begin)
    with tempfile.TemporaryDirectory(prefix="ttt2-switch-") as temp:
        folder = Path(temp)
        emitter = compile_one(folder, "emitter", EMITTER.replace("@CASE@", source[begin:end]),
                              [RECOMP / "thirdparty/fmt/include"])
        generated = subprocess.run([str(emitter)], check=True, text=True, capture_output=True).stdout
        if "PPCUnrecognizedSwitch(0x82ABD378u, ctx.r11.u64);" not in generated:
            raise AssertionError("Production emitter did not preserve switch PC/index in its default")
        for i, header in enumerate([RECOMP / "XenonUtils", ROOT / "re/ppc"]):
            exe = compile_one(folder, f"bounds{i}", CHECKER.replace("@GENERATED@", generated),
                              [header, RECOMP / "thirdparty/simde"])
            for index in (0, 1):
                result = subprocess.run([str(exe), str(index)], text=True, capture_output=True, timeout=10)
                assert result.returncode == 0 and result.stdout.strip() == f"CASE={index}" and not result.stderr
            for index in (2, 0x100000000, 0xFFFFFFFFFFFFFFFF):
                result = subprocess.run([str(exe), f"{index:X}"], text=True, capture_output=True, timeout=10)
                diagnostic = f"[runtime] unrecognized switch: pc=82ABD378 index={index:016X}"
                assert result.returncode != 0, "Invalid switch index returned successfully"
                assert diagnostic in result.stderr, result.stderr
                assert "CASE=" not in result.stdout, "Invalid index executed a case"
            print(f"PASS {header}: valid cases unchanged; three out-of-range indices log and terminate")


if __name__ == "__main__":
    main()
