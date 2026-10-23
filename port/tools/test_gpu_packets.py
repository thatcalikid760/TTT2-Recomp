"""Execute the production PM4 decoder and circular-ring consumer on synthetic data.

Rendering, allocation and guest memory are test adapters. Packet framing, bounds
checks, type-1 decoding, wrapped packet copies, doorbells and rptr updates are
extracted verbatim from kernel_video.cpp; no game instructions/data are required.
"""

from pathlib import Path
import os
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]

ADAPTERS = r'''
#include "gpu_ring_reader.h"
#include "bounded_format.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
struct Fault { int code; };
void* GetCurrentProcess() { return nullptr; }
void TerminateProcess(void*, int code) { throw Fault{code}; }
std::array<uint8_t,0x80000> memory{};
uint8_t* g_base=memory.data();
uint32_t doorbell=0;
uint32_t GRead32(uint8_t* base, uint32_t address) {
    if(address==0x7FC80714u) return doorbell;
    if(uint64_t(address)+4>memory.size()) throw std::runtime_error("out-of-range guest read");
    uint32_t value;std::memcpy(&value,base+address,4);return __builtin_bswap32(value);
}
void GWrite32(uint8_t* base, uint32_t address, uint32_t value) {
    if(uint64_t(address)+4>memory.size()) throw std::runtime_error("out-of-range guest write");
    value=__builtin_bswap32(value);std::memcpy(base+address,&value,4);
}
uint32_t GuestAddr(uint8_t* base, void* address) { return uint32_t(static_cast<uint8_t*>(address)-base); }
struct Heap {
    unsigned allocations=0;
    uint32_t nextShader=0x30000;
    void* Alloc(size_t size, size_t) {
        ++allocations;
        if(size==GpuMaxPacketBytes) return g_base+0x10000;
        if(size==0x10000) {uint32_t at=nextShader;nextShader+=0x10000;return g_base+at;}
        throw std::runtime_error("unexpected allocation size");
    }
} g_virtualHeap;
std::atomic<uint32_t> g_ringBase{0x1000},g_ringSize{32},g_ringRptr{0},g_rptrWriteBack{0x100};
std::atomic<uint32_t> g_frontBufferPhys{0},g_frontBufferSwizzle{0};
int GpuHwMode() { return 0; }
void HwPresentFrame(uint32_t) { throw std::runtime_error("unexpected hardware present"); }
void FrameLogSwap() {}
void PhysMirrorInvalidateAll() {}
void PhysMirrorInvalidate(uint32_t, uint32_t) {}
void FrameLogDraw(uint32_t) {}
uint32_t g_regFile[0x8000]{},g_opcodeSeen[128]{},g_primSeen[64]{};
uint32_t g_curVs=0,g_curPs=0,g_curVsSize=0,g_curPsSize=0,g_regSeq=0;
int g_cwBudget=0,g_pm4Trace=0;
unsigned writes=0,interrupts=0,waits=0;
uint32_t PhysToGuest(uint32_t address) { return address; }
void WriteReg(uint32_t reg,uint32_t value) {
    if(reg>=0x8000) throw std::runtime_error("bad register");
    g_regFile[reg]=value;++writes;
}
bool CwOn() { return false; }
void DumpShaderBlob(uint32_t,uint32_t,uint32_t) {}
void DumpDrawState(uint32_t,uint32_t) {}
void IssueResolve() { throw std::runtime_error("unexpected resolve"); }
struct GpuShader { uint32_t address,size; };
unsigned draws=0;uint32_t lastIndexAddr=0;bool lastIndex32=false;
void GpuExecuteDraw(const uint32_t*,uint32_t,uint32_t indexAddr,bool index32,GpuShader,GpuShader) {
    ++draws;lastIndexAddr=indexAddr;lastIndex32=index32;
}
void RaiseCpInterrupt(uint32_t) { ++interrupts; }
bool WaitRegMem(uint32_t,uint32_t,uint32_t,uint32_t) { ++waits;return false; }
@PRODUCTION@

static unsigned checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL line%d %s\n",__LINE__,#x);return 1;}}while(0)
void RingWords(uint32_t offset, std::initializer_list<uint32_t> words) {
    for(auto word:words) {GWrite32(g_base,g_ringBase+offset,word);offset=(offset+4)&(g_ringSize-1);}
}
void LinearWords(uint32_t address,std::initializer_list<uint32_t> words) {
    for(auto word:words) {GWrite32(g_base,address,word);address+=4;}
}
int main() {
    const uint32_t type1=0x40000000u|(0x578u<<11)|0x100;
    // A type-1 payload deliberately resembles packet headers. It must be DATA.
    LinearWords(0x500,{type1,0xC0005400,0x000001C5,0x123,0xDEADBEEF,0x80000000});
    ExecPacketStream(0x500,24,0);
    CHECK(writes==3 && interrupts==0);
    CHECK(g_regFile[0x100]==0xC0005400 && g_regFile[0x578]==0x1C5 && g_regFile[0x123]==0xDEADBEEF);

    // Header is the last word; both type-1 payload words are at the ring start.
    g_ringRptr=28;doorbell=4;
    RingWords(28,{type1,0x12345678,0xABCDEF01,0x123,0x98765432});
    ConsumeRing();
    CHECK(g_ringRptr==16 && g_regFile[0x100]==0x12345678 && g_regFile[0x578]==0xABCDEF01);
    CHECK(g_regFile[0x123]==0x98765432 && g_virtualHeap.allocations==1);
    CHECK(GRead32(g_base,0x100)==0); // Existing block-quantized rptr writeback.
    // Only the last payload word wraps, and the same staging allocation is reused.
    g_ringRptr=24;doorbell=1;RingWords(24,{type1,0x456,0x789});ConsumeRing();
    CHECK(g_ringRptr==4 && g_regFile[0x100]==0x456 && g_regFile[0x578]==0x789);
    CHECK(g_virtualHeap.allocations==1);
    // A valid doorbell of zero must consume the last complete packet at ring end.
    g_ringRptr=20;doorbell=0;RingWords(20,{type1,0xA,0xB});ConsumeRing();
    CHECK(g_ringRptr==0 && g_regFile[0x100]==0xA && g_regFile[0x578]==0xB);
    // The actual title uses three-word indirect-buffer packets across this edge.
    LinearWords(0x600,{0x578,0x42});
    g_ringRptr=28;doorbell=2;RingWords(28,{0xC0013F00,0x600,2});ConsumeRing();
    CHECK(g_ringRptr==8 && g_regFile[0x578]==0x42);
    // Raw-pointer doorbells and contiguous packets still work.
    g_ringRptr=0;doorbell=g_ringBase+8;RingWords(0,{0x123,0x999});ConsumeRing();
    CHECK(g_ringRptr==8 && g_regFile[0x123]==0x999);

    // A wrapped immediate shader upload must survive reuse of the ring staging
    // by later packets, and vertex/pixel stages need independent owned buffers.
    g_ringSize=64;g_ringRptr=60;doorbell=4;
    RingWords(60,{0xC0032B00,0,2,0x11223344,0x55667788});ConsumeRing();
    const uint32_t vs=g_curVs;
    CHECK(g_curVsSize==2 && GRead32(g_base,vs)==0x11223344);
    g_ringRptr=60;doorbell=4;
    RingWords(60,{0xC0032B00,1,2,0x99AABBCC,0xDDEEFF00});ConsumeRing();
    const uint32_t ps=g_curPs;
    CHECK(ps!=vs && g_curPsSize==2 && GRead32(g_base,vs+4)==0x55667788);
    CHECK(GRead32(g_base,ps)==0x99AABBCC);
    g_ringRptr=60;doorbell=2;RingWords(60,{type1,0x555,0x666});ConsumeRing();
    CHECK(GRead32(g_base,vs)==0x11223344 && GRead32(g_base,ps+4)==0xDDEEFF00);
    g_ringRptr=60;doorbell=4;
    RingWords(60,{0xC0032B00,0,2,0xABABABAB,0xCDCDCDCD});ConsumeRing();
    CHECK(g_curVs==vs && GRead32(g_base,vs)==0xABABABAB);
    CHECK(g_curPs==ps && GRead32(g_base,ps)==0x99AABBCC && g_virtualHeap.allocations==3);
    g_ringSize=32;

    unsigned before=writes;
    g_ringRptr=28;doorbell=1;RingWords(28,{type1,1}); // One required word isn't published.
    bool rejected=false;
    try {ConsumeRing();} catch(const Fault& f) {rejected=f.code==6;}
    CHECK(rejected && writes==before && g_ringRptr==28);
    // Helpers reject malformed geometry and incomplete packets before dispatch.
    auto read=[](uint32_t){return 0u;};auto dispatch=[](const GpuRingPacket&){throw std::runtime_error("unexpected dispatch");};
    CHECK(WalkGpuRing(24,0,4,read,dispatch).status==GpuRingStatus::InvalidRange);
    CHECK(WalkGpuRing(32,2,4,read,dispatch).status==GpuRingStatus::InvalidRange);
    CHECK(WalkGpuRing(32,32,4,read,dispatch).status==GpuRingStatus::InvalidRange);
    CHECK(WalkGpuRing(32,0,0,read,dispatch).status==GpuRingStatus::Complete);
    CHECK(GpuPacketBytes(0xFFFF1000)==GpuMaxPacketBytes);

    // The production decoder's strict gate rejects each framing error BEFORE
    // register writes, interrupts, waits or swap-identifier side effects.
    const uint32_t bad[]={0x00010123,type1,0xC0043C00,0x54535750,0xC0000000,0xC0007F00};
    for(auto header:bad) {
        LinearWords(0x700,{header,0x11,0x22,0x33,0x44});
        rejected=false;before=writes;
        try {ExecPacketStream(0x700,4,0);} catch(const Fault& f) {rejected=f.code==6;}
        CHECK(rejected && writes==before && waits==0 && interrupts==0);
    }
    // Invalid-opcode diagnostics are optional; bounds protection is always on.
    LinearWords(0x700,{0xC0000000,0});rejected=false;
    try {ExecPacketStream(0x700,8,0);} catch(const Fault& f) {rejected=f.code==6;}
    CHECK(rejected==(std::getenv("TTT2_STRICT_GPU")!=nullptr));
    CHECK(draws==0); // nothing above is a draw
    // DRAW_INDX's index base is aligned to the index size only. TTT2 packs 16-bit strips
    // back to back, so half start on a 2-byte boundary; rounding to 4 bytes made the
    // previous strip's last index each strip's first vertex (the streak lines).
    LinearWords(0x7400,{0xC0032200,0,(3u<<16)|4u,0x1236,3});
    ExecPacketStream(0x7400,20,0);
    CHECK(draws==1 && lastIndexAddr==0x1236 && !lastIndex32);
    LinearWords(0x7500,{0xC0032200,0,(3u<<16)|(1u<<8)|4u,0x1238,3});
    ExecPacketStream(0x7500,20,0);
    CHECK(draws==2 && lastIndexAddr==0x1238 && lastIndex32);
    std::printf("PASS %u PM4 checks: type1, wrapping, IB dispatch, doorbell zero, bounds before side effects, index base alignment\n",checks);
}
'''


def main():
    video = (ROOT / "port/src/runtime/kernel/kernel_video.cpp").read_text(encoding="utf-8-sig")
    begin = video.index("struct PacketHistoryEntry")
    end = video.index("\n// The CP thread:", begin)
    source = ADAPTERS.replace("@PRODUCTION@", video[begin:end])
    with tempfile.TemporaryDirectory(prefix="ttt2-gpu-packets-") as temp:
        folder = Path(temp)
        exe = compile_one(folder, "packets", source, [ROOT / "port/src/runtime/kernel"])
        for strict in (False, True):
            env = dict(os.environ)
            env.pop("TTT2_STRICT_GPU", None)
            if strict:
                env["TTT2_STRICT_GPU"] = "1"
            result = subprocess.run([str(exe)], env=env, text=True, capture_output=True, timeout=15)
            print(f"strict={int(strict)}: {result.stdout}", end="")
            if result.returncode:
                print(result.stderr)
                raise SystemExit(result.returncode)
        print("PASS malformed packet bounds rejected with and without strict diagnostics")


if __name__ == "__main__":
    main()
