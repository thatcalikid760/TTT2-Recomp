"""Check the production MFTB clock, kernel frequency ABI, and old-code controls."""
from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>
#include "@TIME_HEADER@"
@CLOCK@
union Register { uint32_t u32; uint64_t u64; };
struct PPCContext { Register r3{},r4{}; };
#define PPC_FUNC(name) void name(PPCContext& ctx,uint8_t* base)
void KTrace(const char*) {}
@IMPORT@
unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)) { std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
    const uint64_t cases[]={0,1,19,20,21,999999999,1000000000,
        85899345919ull,85899345920ull,UINT64_MAX};
    for(auto ns:cases) CHECK(GuestTimebaseFromNanoseconds(ns)==ns/20);
    // The 32-bit time-base half wraps after 85.89934592 seconds, just as Xenon.
    CHECK(uint32_t(GuestTimebaseFromNanoseconds(85899345920ull))==0);
    CHECK(GuestTimebaseFromNanoseconds(1000000000)==PPC_TIME_BASE_FREQUENCY);
    PPCContext ctx;ctx.r3.u64=0xABCDEF1298765432ull;
    ctx.r4.u64=0xFEDCBA9876543210ull;
    std::array<uint8_t,64> bytes;bytes.fill(0xA5);auto original=bytes;
    __imp__KeQueryPerformanceFrequency(ctx,bytes.data());
    CHECK(ctx.r3.u64==50000000); // real guest wrapper uses std r3,0(r31)
    CHECK(ctx.r4.u64==0xFEDCBA9876543210ull);
    CHECK(bytes==original);
    std::atomic<bool> backwards=false;
    std::vector<std::thread> workers;
    for(int i=0;i<4;++i) workers.emplace_back([&]{
        auto last=PPCReadTimebase();
        for(int j=0;j<10000;++j) {
            auto now=PPCReadTimebase();
            if(now<last) backwards=true;
            last=now;
        }
    });
    for(auto& t:workers) t.join();
    CHECK(!backwards.load());
    LARGE_INTEGER frequency,a,b;CHECK(QueryPerformanceFrequency(&frequency));
    QueryPerformanceCounter(&a);const auto first=PPCReadTimebase();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto last=PPCReadTimebase();QueryPerformanceCounter(&b);
    const double host=double(b.QuadPart-a.QuadPart)/frequency.QuadPart;
    const double guest=double(last-first)/ctx.r3.u64;
    const double ratio=guest/host;
    if(ratio<0.99||ratio>1.01) std::fprintf(stderr,"clock ratio=%f\n",ratio);
    CHECK(ratio>=0.99&&ratio<=1.01);
    std::printf("PASS %u timing checks plus 40000 monotonic reads: 50MHz, ABI, host elapsed ratio %.6f\n",checks,ratio);
}
'''


def main():
    runtime=ROOT/'port/src/runtime'
    sys=(runtime/'kernel/kernel_sys.cpp').read_text(encoding='utf-8-sig')
    start=sys.index('PPC_FUNC(__imp__KeQueryPerformanceFrequency)')
    production=sys[start:sys.index('\n}',start)+2]
    clock=(runtime/'guest_time.cpp').read_text().replace('#include "guest_time.h"','')
    template=HARNESS.replace('@TIME_HEADER@',(runtime/'guest_time.h').as_posix())
    with tempfile.TemporaryDirectory(prefix='ttt2-time-') as tmp:
        folder=Path(tmp)
        variants=[('current',production,clock,None),
            ('old_abi','PPC_FUNC(__imp__KeQueryPerformanceFrequency) { ctx.r3.u32=0; ctx.r4.u32=50000000; }',clock,'ctx.r3.u64==50000000'),
            ('old_tsc',production,'#include <chrono>\nuint64_t PPCReadTimebase() { return __rdtsc(); }','clock ratio=')]
        for name,imp,clk,error in variants:
            source=template.replace('@IMPORT@',imp).replace('@CLOCK@',clk)
            exe=compile_one(folder,name,source,[])
            result=subprocess.run([str(exe)],text=True,capture_output=True,timeout=10)
            if error:
                if result.returncode==0 or error not in result.stderr:
                    raise RuntimeError(name+' negative control did not fail as expected:\n'+result.stdout+result.stderr)
                print('PASS negative control:',name)
            elif result.returncode:
                raise RuntimeError(result.stdout+result.stderr)
            else:
                print(result.stdout,end='')
    # Verify all current generated MFTB sites use the runtime hook. This also
    # detects stale generation after reverting just the compiler change.
    count=0
    for p in (ROOT/'re/ppc').glob('ppc_recomp.*.cpp'):
        text=p.read_text(encoding='utf-8')
        assert '= __rdtsc();' not in text,p
        count+=text.count('= PPCReadTimebase();')
    assert count>0
    compiler=(ROOT/'tools/XenonRecomp/XenonRecomp/recompiler.cpp').read_text()
    body=compiler.split('case PPC_INST_MFTB:',1)[1].split('break;',1)[0]
    assert 'PPCReadTimebase()' in body
    print(f'PASS {count} generated MFTB sites and durable recompiler hook')


if __name__=='__main__':
    main()
