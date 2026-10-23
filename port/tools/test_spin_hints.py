"""Verify actual DB16CYC emission and the generated GPU fence spin region.

IBM/Sony/Toshiba CBEA language extensions v2.5, pp.80-82, define dispatch
priority hints and dispatch delays without register effects. PAUSE supplies a
host spin hint, not sixteen host cycles or an OS scheduling guarantee:
https://informatica.uv.es/iiguia/AAC/ps3/documentacion/Language_Extensions_for_CBEA_2.5.pdf

Compiles only extracted production code. No full regeneration, build or game.
"""

from pathlib import Path
import re
import subprocess
import tempfile

from test_vcmpgtuh import ROOT, RECOMP, compile_one


EMITTER = r'''
#include <cstdio>
#include <string>
#define FMT_HEADER_ONLY
#include <fmt/format.h>
std::string output;
template<class... Args> void println(fmt::format_string<Args...> pattern, Args&&... args) {
    output += fmt::format(pattern, std::forward<Args>(args)...) + "\n";
}
enum {PPC_INST_CCTPL, PPC_INST_CCTPM, PPC_INST_DB16CYC};
void emit(int instruction) { switch(instruction) {
@CASES@
} }
int main() {
    for(int instruction=0;instruction<3;++instruction) {
        output.clear(); emit(instruction);
        std::printf("__declspec(noinline) void op%d(PPCContext& ctx) {\n%s}\n",instruction,output.c_str());
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
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstring>
@GENERATED@
thread_local unsigned pauses=0;
void countedPause() { ++pauses; _mm_pause(); }
#define _mm_pause countedPause
void productionDelay(PPCContext& ctx) {
@DELAY@
}
#undef _mm_pause
unsigned checks=0;
#define CHECK(value) do {++checks;if(!(value)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#value);return 1;}}while(0)
int main() {
    PPCContext ctx{},expected{};
    uint64_t random=0x123456789abcdef0ull;
    for(unsigned seed=0;seed<256;++seed) {
        auto* bytes=reinterpret_cast<unsigned char*>(&ctx);
        for(size_t i=0;i<sizeof(ctx);++i) {
            random^=random<<13;random^=random>>7;random^=random<<17;
            bytes[i]=static_cast<unsigned char>(random);
        }
        std::memcpy(&expected,&ctx,sizeof(ctx));
        op0(ctx);CHECK(std::memcmp(&ctx,&expected,sizeof(ctx))==0);
        op1(ctx);CHECK(std::memcmp(&ctx,&expected,sizeof(ctx))==0);
        op2(ctx);CHECK(std::memcmp(&ctx,&expected,sizeof(ctx))==0);
        expected.r11.s64=4;expected.ctr.u64=0;
        pauses=0;productionDelay(ctx);
        CHECK(pauses==32);
        CHECK(std::memcmp(&ctx,&expected,sizeof(ctx))==0);
    }
    // Completion is written only by the producer. The real delay region must
    // keep polling, neither satisfying the fence nor preventing its progress.
    std::atomic<unsigned> entered{0},completed{0};
    std::thread producer([&] {
        for(unsigned target=1;target<=128;++target) {
            while(entered.load(std::memory_order_acquire)<target) _mm_pause();
            completed.store(target,std::memory_order_release);
        }
    });
    bool progress=true;
    for(unsigned target=1;target<=128;++target) {
        if(completed.load(std::memory_order_acquire)>=target) progress=false;
        entered.store(target,std::memory_order_release);
        do {productionDelay(ctx);} while(completed.load(std::memory_order_acquire)<target);
        if(completed.load(std::memory_order_acquire)!=target) progress=false;
    }
    producer.join();CHECK(progress);CHECK(completed.load()==128);
    std::printf("PASS %u checks: complete guest context preservation, actual 32-hint delay loop, 128 producer/completion handshakes\n",checks);
}
'''


def main():
    source=(RECOMP / 'XenonRecomp/recompiler.cpp').read_text(encoding='utf-8-sig')
    priority=re.search(r'    case PPC_INST_CCTPL:\n.*?        break;',source,re.S)
    delay=re.search(r'    case PPC_INST_DB16CYC:\n.*?        break;',source,re.S)
    assert priority and delay and 'case PPC_INST_CCTPM:' in priority.group()
    chunks={}
    variants={}
    production=None
    for path in sorted((ROOT / 're/ppc').glob('ppc_recomp.*.cpp')):
        text=path.read_text(encoding='utf-8-sig')
        for match in re.finditer(r'\t// (db\d+cyc|cctp[lmh]) *\n',text):
            kind=match[1];variants[kind]=variants.get(kind,0)+1
            if kind=='db16cyc':
                assert text[match.end():].startswith('\t_mm_pause();\n'),str(path)
                chunks[path.name]=chunks.get(path.name,0)+1
        if 'PPC_FUNC_IMPL(__imp__sub_8273D4A0)' in text:
            function=text.split('PPC_FUNC_IMPL(__imp__sub_8273D4A0)',1)[1].split('\n}',1)[0]
            production=function[function.index('\t// cctpl'):function.index('\t// lbz r11,11069')]
    assert variants=={'cctpl':1,'db16cyc':20,'cctpm':1},variants
    assert production and sum(chunks.values())==20
    print('PASS generated inventory:',variants, chunks)
    with tempfile.TemporaryDirectory(prefix='ttt2-spin-') as temp:
        folder=Path(temp)
        emitter=compile_one(folder,'emit',EMITTER.replace('@CASES@',priority.group()+'\n'+delay.group()),[RECOMP/'thirdparty/fmt/include'])
        generated=subprocess.check_output([str(emitter)],text=True)
        assert generated.count('_mm_pause();')==1,generated
        text=CHECKER.replace('@GENERATED@',generated).replace('@DELAY@',production)
        includes=[RECOMP/'XenonUtils',RECOMP/'thirdparty/simde']
        checker=compile_one(folder,'check',text,includes)
        result=subprocess.run([str(checker)],text=True,capture_output=True,timeout=30)
        print(result.stdout+result.stderr,end='')
        if result.returncode:raise SystemExit(result.returncode)
        assembly=subprocess.check_output([r'C:\Program Files\LLVM\bin\llvm-objdump.exe','-d',str(folder/'check.obj')],text=True)
        op2=re.search(r'<\?op2.*?:\n(.*?)(?:\n\n|\Z)',assembly,re.S)
        assert op2 and re.search(r'\bpause\b',op2[1]),'No host PAUSE in actual emitted op2 assembly'
        print('PASS real host PAUSE emitted at /O2 using existing production headers')
        # Reproduce old generated no-op behavior; the dynamic loop-count check
        # must fail even though its registers still end at the correct values.
        old=text.replace('\t_mm_pause();\n','')
        negative=compile_one(folder,'old_noop',old,includes)
        result=subprocess.run([str(negative)],text=True,capture_output=True,timeout=30)
        assert result.returncode and 'pauses==32' in result.stderr,result.stdout+result.stderr
        print('PASS old no-op negative control detected')


if __name__=='__main__':main()
