"""Test extracted RLDICL emitter against a per-bit scalar oracle and 64-bit CR0.

Uses the production ComputeMask/emitter and PPCRegister/PPCCRRegister helpers.
No full recompiler build, game regeneration, or game execution is performed.
"""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from test_vcmpgtuh import RECOMP, compile_one


EMITTER = r'''
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#define FMT_HEADER_ONLY
#include <fmt/format.h>
@MASK@
std::string output;
template<class... Args> void println(fmt::format_string<Args...> pattern, Args&&... args) {
    output += fmt::format(pattern, std::forward<Args>(args)...) + "\n";
}
std::string r(int i) { return fmt::format("regs[{}]",i); }
std::string cr(int i) { return fmt::format("cr{}",i); }
std::string xer() { return "xer"; }
struct Opcode { const char* name; };
struct Instruction { Opcode* opcode; int operands[4]; };
constexpr int PPC_INST_RLDICL=0;
void emit(Instruction insn) { switch(0) {
@CASE@
} }
int main() {
    const int edges[]={0,1,31,32,33,63}; int index=0;
    for(int sh:edges) for(int mb:edges) for(int record=0;record<2;++record)
    for(int alias=0;alias<2;++alias) {
        Opcode op{record?"rldicl.":"rldicl"};
        Instruction insn{&op,{alias,1,sh,mb}};
        output.clear();emit(insn);
        std::printf("void op%d(PPCRegister* regs, PPCCRRegister& cr0, const PPCXERRegister& xer) {\n%s}\n",index++,output.c_str());
    }
    std::printf("Case testCases[]={\n");index=0;
    for(int sh:edges) for(int mb:edges) for(int record=0;record<2;++record)
    for(int alias=0;alias<2;++alias)
        std::printf("{op%d,%d,%d,%d,%d},\n",index++,sh,mb,record,alias);
    std::printf("};\n");
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
using Fn=void(*)(PPCRegister*,PPCCRRegister&,const PPCXERRegister&);
struct Case {Fn fn; int shift,maskBegin,record,destination;};
@GENERATED@
unsigned count=0;
bool check(uint64_t input) {
    for(auto test:testCases) for(int so=0;so<2;++so) for(int seed=0;seed<16;++seed) {
        PPCRegister regs[2]{}; regs[0].u64=0xAA55AA55AA55AA55ull;regs[1].u64=input;
        PPCRegister expected[2];std::memcpy(expected,regs,sizeof(regs));
        uint64_t value=0;
        // IBM numbers the most significant bit zero. Rotate individual source
        // bits and retain only destination bits numbered maskBegin..63.
        for(int bit=0;bit<64;++bit) {
            int destination=(bit+test.shift)%64;
            if(63-destination>=test.maskBegin && ((input>>bit)&1)) value|=uint64_t(1)<<destination;
        }
        expected[test.destination].u64=value;
        PPCCRRegister cr0{};cr0.lt=seed&1;cr0.gt=(seed>>1)&1;cr0.eq=(seed>>2)&1;cr0.so=(seed>>3)&1;
        PPCCRRegister wanted=cr0;
        PPCXERRegister xer{};xer.so=so;xer.ov=1;xer.ca=1;
        if(test.record) {wanted.lt=value>>63;wanted.eq=value==0;wanted.gt=!wanted.lt&&!wanted.eq;wanted.so=so;}
        test.fn(regs,cr0,xer);++count;
        if(std::memcmp(regs,expected,sizeof(regs)) || std::memcmp(&cr0,&wanted,sizeof(cr0)) || xer.so!=so || xer.ov!=1 || xer.ca!=1) {
            std::fprintf(stderr,"FAIL case=%u input=%016llX sh=%d mb=%d Rc=%d alias=%d CR0=%u%u%u%u expected=%u%u%u%u value=%016llX\n",
                count,(unsigned long long)input,test.shift,test.maskBegin,test.record,test.destination,
                cr0.lt,cr0.gt,cr0.eq,cr0.so,wanted.lt,wanted.gt,wanted.eq,wanted.so,(unsigned long long)value);
            return false;
        }
    }
    return true;
}
int main() {
    const uint64_t edges[]={0,1,0x7FFFFFFF,0x80000000,0xFFFFFFFF,0x100000000,0x7FFFFFFFFFFFFFFFull,0x8000000000000000ull,0xFFFFFFFFFFFFFFFFull};
    for(auto input:edges) if(!check(input))return 1;
    uint64_t random=0x123456789ABCDEF0ull;
    for(int i=0;i<128;++i) {random^=random<<13;random^=random>>7;random^=random<<17;if(!check(random))return 1;}
    std::printf("PASS %u scalar-oracle cases: rotate/mask edges, 64-bit signs, Rc=0/1, XER.SO, all initial CR0 fields, source/destination aliasing\n",count);
}
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--negative-control", choices=["missing-cr0", "narrow-cr0"])
    args=parser.parse_args()
    source=(RECOMP / "XenonRecomp/recompiler.cpp").read_text(encoding="utf-8-sig")
    case=re.search(r"    case PPC_INST_RLDICL:\n.*?        break;",source,re.S)
    mask=re.search(r"static uint64_t ComputeMask\(.*?\n\}",source,re.S)
    if not case or not mask: raise SystemExit("Production emitter/ComputeMask not found")
    with tempfile.TemporaryDirectory(prefix="ttt2-rldicl-") as temp:
        folder=Path(temp)
        emitter=compile_one(folder,"emit",EMITTER.replace("@CASE@",case.group()).replace("@MASK@",mask.group()),[RECOMP / "thirdparty/fmt/include"])
        generated=subprocess.check_output([str(emitter)],text=True)
        if args.negative_control=="missing-cr0":generated=re.sub(r"^.*compare<int64_t>.*\n","",generated,flags=re.M)
        if args.negative_control=="narrow-cr0":generated=generated.replace("compare<int64_t>","compare<int32_t>").replace(".s64, 0,", ".s32, 0,")
        checker=compile_one(folder,"check",CHECKER.replace("@GENERATED@",generated),[RECOMP / "XenonUtils",RECOMP / "thirdparty/simde"])
        result=subprocess.run([str(checker)],text=True,capture_output=True)
        print(result.stdout+result.stderr,end="")
        if args.negative_control:
            if result.returncode==0:raise SystemExit("Negative control unexpectedly passed")
            print(f"PASS negative control: {args.negative_control} detected")
        elif result.returncode:raise SystemExit(result.returncode)


if __name__=="__main__":main()
