"""Compile the production draw profiler; test clocks, ranking, bounds and opt-in.

No game/device or sleeping is needed. Aggregation receives synthetic timestamps;
separate subprocesses exercise the real RAII scope and cached environment flag.
"""
import os
from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / 'port/src/runtime/kernel'

CHECKER = r'''
#include "gpu_draw_profile.h"
#include <string>
using namespace gpu_draw_profile_detail;
unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;} }while(0)
static void EarlyReturn(const uint32_t* words) {
    GpuDrawProfileScope scope(words,2,6,3);
    return;
}
int main(int argc,char** argv) {
    if(argc>1 && std::strcmp(argv[1],"disabled")==0) {
        // A disabled profiler must not read shader memory or initialize timing.
        GpuDrawProfileScope scope(reinterpret_cast<const uint32_t*>(1),100,4,6);
        Report r; CHECK(!SharedAccumulator().ReportIfDue(UINT64_MAX,r));
        std::puts("PASS disabled scope does not touch shader/accumulator");return 0;
    }
    if(argc>1 && std::strcmp(argv[1],"scope")==0) {
        uint32_t words[]={0x11223344,0xAABBCCDD};const auto hash=ShaderHash(words,2);
        EarlyReturn(words);
        { GpuDrawProfileScope scope(words,2,6,9); words[1]^=1; }
        Report r;CHECK(SharedAccumulator().ReportIfDue(UINT64_MAX,r));
        CHECK(r.total.draws==2 && r.total.indices==12 && r.families==1);
        CHECK(r.top[0].shaderHash==hash && r.top[0].totals.draws==2);
        CHECK(r.total.maxNs<=r.total.wallNs);
        std::puts("PASS enabled RAII: early-return, copied hash, actual monotonic durations");return 0;
    }
    CHECK(!EnabledValue(nullptr) && !EnabledValue("") && !EnabledValue("0"));
    CHECK(!EnabledValue("true") && !EnabledValue("01") && EnabledValue("1"));
    uint32_t words[]={0x11223344,0xAABBCCDD};const auto hash=ShaderHash(words,2);
    CHECK(ShaderHash(nullptr,0)==14695981039346656037ull);
    CHECK(hash==@REFERENCE_HASH@ull);
    words[1]^=0x80000000;CHECK(ShaderHash(words,2)!=hash);
    Accumulator a;Report r;
    CHECK(!a.ReportIfDue(UINT64_MAX,r));
    a.Record(10,33,4,6,100,200);a.Record(10,33,4,9,200,500);
    a.Record(11,44,6,3,500,1500);
    // Same hash/length but a different primitive is a separate ranked family.
    a.Record(10,33,6,3,1500,1700);
    CHECK(!a.ReportIfDue(100+Accumulator::IntervalNs-1,r));
    CHECK(a.ReportIfDue(100+Accumulator::IntervalNs,r));
    CHECK(r.total.draws==4 && r.total.indices==21 && r.total.wallNs==1600 && r.total.maxNs==1000);
    CHECK(r.families==3 && r.topCount==3 && r.overflow.draws==0);
    CHECK(r.top[0].shaderHash==11 && r.top[0].totals.wallNs==1000);
    CHECK(r.top[1].shaderHash==10 && r.top[1].primitive==4 && r.top[1].totals.draws==2);
    CHECK(r.top[1].totals.indices==15 && r.top[1].totals.wallNs==400 && r.top[1].totals.maxNs==300);
    CHECK(r.top[2].primitive==6 && r.elapsedNs==Accumulator::IntervalNs);
    auto file=std::tmpfile();CHECK(file);PrintReport(r,file);std::rewind(file);
    char text[4096]{};const auto length=std::fread(text,1,sizeof(text)-1,file);std::fclose(file);
    std::string log(text,length);
    CHECK(log.find("draw_wall_ms=")!=std::string::npos && log.find("rank=1 ps_hash=000000000000000B ps_words=44 prim=6")!=std::string::npos);
    CHECK(log.find("share_pct=62.50")!=std::string::npos);
    // Reporting resets both totals and groups; no old hashes leak into the window.
    a.Record(99,1,4,3,100+Accumulator::IntervalNs,200+Accumulator::IntervalNs);
    CHECK(a.ReportIfDue(100+2*Accumulator::IntervalNs,r));
    CHECK(r.total.draws==1 && r.families==1 && r.top[0].shaderHash==99);
    Accumulator capped;
    for(uint64_t i=0;i<Accumulator::MaxFamilies;++i)
        capped.Record(i*512,3,4,3,i,i+1); // Collision-heavy keys test probing.
    capped.Record(999,3,4,7,1000,2000); // Over-capacity family is aggregated.
    capped.Record(0,3,4,3,2000,4000);   // Existing family still updates at capacity.
    CHECK(capped.ReportIfDue(Accumulator::IntervalNs,r));
    CHECK(r.families==Accumulator::MaxFamilies && r.topCount==8);
    CHECK(r.total.draws==258 && r.total.wallNs==3256 && r.overflow.draws==1 && r.overflow.wallNs==1000);
    CHECK(r.top[0].shaderHash==0 && r.top[0].totals.wallNs==2001 && r.top[0].totals.draws==2);
    // Repeated windows retain no unbounded family registry.
    for(uint64_t window=1;window<=100;++window) {
        const auto t=window*Accumulator::IntervalNs;
        capped.Record(window,33,8,4,t,t+10);
        CHECK(capped.ReportIfDue(t+Accumulator::IntervalNs,r));
        CHECK(r.total.draws==1 && r.families==1 && r.top[0].shaderHash==window);
    }
    std::printf("PASS %u draw-profile checks: full hash, interval, totals/rank, bounded collisions/overflow/reset, formatting\n",checks);
}
'''


def main():
    expected = 14695981039346656037
    for byte in bytes.fromhex('44332211DDCCBBAA'):
        expected = ((expected ^ byte) * 1099511628211) & ((1 << 64) - 1)
    with tempfile.TemporaryDirectory(prefix='ttt2-draw-profile-') as directory:
        folder = Path(directory)
        exe = compile_one(folder, 'profile', CHECKER.replace('@REFERENCE_HASH@', str(expected)), [KERNEL])
        for mode, value in [('aggregate', None), ('disabled', None), ('disabled', ''),
                            ('disabled', '0'), ('disabled', 'true'), ('scope', '1')]:
            env = dict(os.environ)
            env.pop('TTT2_GPU_PROFILE', None)
            if value is not None:
                env['TTT2_GPU_PROFILE'] = value
            result = subprocess.run([str(exe), mode], capture_output=True, text=True, env=env, timeout=20)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            print(result.stdout, end='')


if __name__ == '__main__':
    main()
