"""Exercise the production CPU rasterizer with synthetic microcode and compare counters.

Compiles gpu_render.cpp, adapting only guest memory / fault reporting. A negative
control restores the former shared per-pixel atomics in the same production code;
pixels, depth, counts, budget decisions and uniform-pixel diagnostics must match.
The optional benchmark measures that old contention against row-local totals.
No game instructions or assets are embedded.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / 'port/src/runtime/kernel'

ADAPTER = r'''
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <stdexcept>
uint8_t guest[0x20000]{};
uint8_t* g_base = guest;
uint32_t GuestOffset(uint32_t address) {
    address &= 0x1FFFFFFF;
    if (address >= sizeof(guest)-4) throw std::runtime_error("fixture guest bounds");
    return address;
}
uint8_t GRead8(uint8_t* base, uint32_t address) {return base[GuestOffset(address)];}
uint16_t GRead16(uint8_t* base, uint32_t address) {
    uint16_t v; std::memcpy(&v,base+GuestOffset(address),2);return __builtin_bswap16(v);
}
uint32_t GRead32(uint8_t* base, uint32_t address) {
    uint32_t v; std::memcpy(&v,base+GuestOffset(address),4);return __builtin_bswap32(v);
}
int GuestFaultReport(EXCEPTION_POINTERS*) {return EXCEPTION_CONTINUE_SEARCH;}
bool DiagSkinDescribe(uint32_t,char*,size_t) {return false;}
'''

MAIN = r'''
#include <chrono>
#include <string>
void Put(uint32_t address,uint32_t value) {
    value=__builtin_bswap32(value);std::memcpy(guest+address,&value,4);
}
uint32_t Bits(float f) {uint32_t u;std::memcpy(&u,&f,4);return u;}
void Words(uint32_t address,std::initializer_list<uint32_t> values) {for(auto v:values){Put(address,v);address+=4;}}
uint32_t regs[0x8000]{};
void Configure(int width,int height) {
    std::memset(regs,0,sizeof(regs));
    regs[0x2000]=width;regs[0x2001]=0;regs[0x2002]=0;
    regs[0x2104]=15;regs[0x2206]=0x100;regs[0x2201]=0x00010001;
    regs[0x2082]=(height<<16)|width;
    regs[0x4800]=0x2003;
    regs[0x4000+256*4+0]=Bits(.25f);regs[0x4000+256*4+1]=Bits(.5f);
    regs[0x4000+256*4+2]=Bits(.75f);regs[0x4000+256*4+3]=Bits(.5f);
    // Synthetic VS: fetch float4 position to r1, MAX r1/r1 -> position export62.
    Words(0x100,{1|(2<<12)|(1<<16),2<<12,0,
                 1<<12,(38<<16)|0x688,4,
                 62|0x8000|(15<<16),0,(2u<<24)|(1u<<16)|(1u<<8)|(1u<<31)|(1u<<30)});
    // Synthetic PS: MAX c0/c0 -> colour export0. No input or texture dependency.
    Words(0x200,{1|(1<<12),2<<12,0,0x8000|(15<<16),0,2<<24});
    const float vertices[][4]={{0,0,.25f,1},{float(width),0,.25f,1},{0,float(height),.25f,1},
                              {float(width),0,.25f,1},{float(width),float(height),.25f,1},{0,float(height),.25f,1},
                              {0,0,.75f,1},{float(width),0,.75f,1},{0,float(height),.75f,1},
                              {float(width),0,.75f,1},{float(width),float(height),.75f,1},{0,float(height),.75f,1}};
    for(int i=0;i<12;++i)for(int c=0;c<4;++c)Put(0x2000+i*16+c*4,Bits(vertices[i][c]));
}
uint64_t Hash(const uint32_t* data,size_t size) {
    uint64_t hash=1469598103934665603ull;for(size_t i=0;i<size;++i){hash^=data[i];hash*=1099511628211ull;}return hash;
}
uint64_t lastWritten,lastCovered,lastNonBlack;
uint32_t lastFirstColor,lastFirstDst;
bool fixtureSkipRow=false;unsigned fixtureRowHits=0;
unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc,char** argv) {
    bool benchmark=argc>1 && std::string(argv[1])=="bench";
    if(benchmark) {
        Configure(1280,720);GpuFillEdram(0,1280,720,0xFF112233);
        GpuExecuteDraw(regs,(6<<16)|(2<<6)|4,0,false,{0x100,9},{0x200,6});
        auto start=std::chrono::steady_clock::now();
        for(int i=0;i<8;++i)GpuExecuteDraw(regs,(6<<16)|(2<<6)|4,0,false,{0x100,9},{0x200,6});
        double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/8;
        std::printf("BENCH %.3f ms/fullscreen draw, workers=%u\n",ms,RasterPool::Get().Workers());
        return 0;
    }
    bool limited=std::getenv("TTT2_MAXPIX")!=nullptr;
    for(int test=0;test<13;++test) {
        int width=test==0?32:128,height=test==0?24:96;Configure(width,height);
        fixtureSkipRow=test==12;fixtureRowHits=0;
        uint32_t background=0xFF112233;GpuFillEdram(0,width,height,background);
        g_depth.clear();
        if(test==2)regs[0x2201]=4|(5<<8)|(4<<16)|(5<<24); // alpha blend
        if(test==3)regs[0x2104]=5; // colour write mask
        if(test==4){regs[0x2081]=(8<<16)|7;regs[0x2082]=(70<<16)|90;} // scissor
        if(test==5){regs[0x2200]=2|4|(1<<4);} // less, near then far
        if(test==6){regs[0x2202]=8|4;regs[0x210E]=Bits(.75f);} // alpha reject
        if(test==7){regs[0x2200]=2|4|(7<<4);regs[0x2104]=0;} // depth-only, enabled ALWAYS test
        if(test==8)regs[0x2205]=1; // face cull
        if(test==9){regs[0x4000+256*4]=0;regs[0x4000+256*4+1]=0;regs[0x4000+256*4+2]=0;}
        if(test==10)regs[0x2000]|=2<<16; // 4x samples
        if(test==11){regs[0x2200]=2|4|(7<<4);regs[0x2202]=8|5;regs[0x210E]=Bits(0);}
        bool draw=GpuExecuteDraw(regs,((test==5?12:6)<<16)|(2<<6)|4,0,false,{0x100,9},{0x200,6});
        const auto& surface=GetSurface(0);
        uint64_t zhash=0;auto zi=g_depth.find(0);if(zi!=g_depth.end())zhash=Hash(reinterpret_cast<const uint32_t*>(zi->second.data()),zi->second.size());
        std::printf("CASE %d %d %llu %llu %llu %08X %08X %016llX %016llX\n",test,draw,
            lastCovered,lastWritten,lastNonBlack,lastFirstColor,lastFirstDst,
            Hash(surface.px.data(),surface.px.size()),zhash);
        CHECK(draw==(lastWritten!=0));CHECK(lastNonBlack<=lastWritten);CHECK(lastWritten<=lastCovered);
        if(test==0 || test==1) {
            uint64_t expected=uint64_t(width)*height;
            if(limited && expected>2000)expected/=2;
            CHECK(lastWritten==expected);CHECK(lastCovered==expected);CHECK(lastNonBlack==expected);
            CHECK(lastFirstColor==0x804080BF);CHECK(lastFirstDst==background);
        }
        if(test==6 || test==7)CHECK(lastWritten==0 && !draw);
    }
    std::printf("PASS %u checks\n",checks);
}
'''


def checked_replace(source, old, new):
    if source.count(old)!=1:raise RuntimeError(f'production anchor missing/ambiguous: {old[:75]}')
    return source.replace(old,new,1)


def legacy_counters(source):
    source=checked_replace(source,'\n    uint64_t written = 0, covered = 0, nonBlack = 0;','    std::atomic<uint64_t> written{0}, covered{0}, nonBlack{0};')
    source=checked_replace(source,'if (stats.written++ == 0)','if (written.fetch_add(1, std::memory_order_relaxed) == 0)')
    source=checked_replace(source,'stats.firstColor = packed; stats.firstShaded = src; stats.firstDst = dstBefore;','firstColor = packed; firstShaded = src; firstDst = dstBefore;')
    source=checked_replace(source,'if (packed & 0x00FFFFFF) ++stats.nonBlack;','if (packed & 0x00FFFFFF) nonBlack.fetch_add(1, std::memory_order_relaxed);')
    source=checked_replace(source,'++stats.covered;','++covered;')
    start=source.index('        // Pick the first written pixel in row order')
    end=source.index('\n    };',start)
    return source[:start]+source[end:]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--benchmark',action='store_true')
    args=parser.parse_args()
    source=(KERNEL/'gpu_render.cpp').read_text()
    # Recording completed diagnostics does not alter rasterization or shader execution.
    source=checked_replace(source,'    return written != 0;','''    lastWritten=written;lastCovered=covered;lastNonBlack=nonBlack;
    lastFirstColor=firstColor;lastFirstDst=firstDst;
    return written != 0;''')
    source=checked_replace(source,'        auto rowFn = [&](int y)\n        {','''        auto rowFn = [&](int y)
        {
            // Simulate a row abandoned by the existing worker fault guard.
            if(fixtureSkipRow && y==minY && fixtureRowHits++ != 0)return;''')
    source='extern bool fixtureSkipRow; extern unsigned fixtureRowHits;\nextern unsigned long long lastWritten,lastCovered,lastNonBlack;\nextern unsigned int lastFirstColor,lastFirstDst;\n'+source
    with tempfile.TemporaryDirectory(prefix='ttt2-raster-') as temporary:
        folder=Path(temporary);(folder/'kernel_shared.h').write_text(ADAPTER)
        (folder/'gpu_render.h').write_text((KERNEL/'gpu_render.h').read_text())
        executables={}
        for name,code in [('rows',source),('atomics',legacy_counters(source))]:
            executables[name]=compile_one(folder,name,code+MAIN,[folder])
        for limited in (False,True):
            env=dict(os.environ,TTT2_RENDERLOG='0',TTT2_PRIMLOG='0')
            env.pop('TTT2_MAXPIX',None)
            if limited:env['TTT2_MAXPIX']='2000'
            outputs={}
            for name,exe in executables.items():
                result=subprocess.run([str(exe)],capture_output=True,text=True,env=env,timeout=40)
                if result.returncode:raise RuntimeError(name+'\n'+result.stdout+result.stderr)
                outputs[name]=result.stdout
            if outputs['rows']!=outputs['atomics']:raise RuntimeError('Raster parity mismatch\n'+repr(outputs))
            print(f'PASS pixel/depth/diagnostic/budget parity (limited={limited}): '+outputs['rows'].splitlines()[-1])
        # A lost row merge must fail both the scalar invariants and parity checks.
        broken=checked_replace(source,'            covered += stats.covered;','            // negative control: covered merge omitted')
        broken_exe=compile_one(folder,'missing_merge',broken+MAIN,[folder])
        result=subprocess.run([str(broken_exe)],capture_output=True,text=True,env=env,timeout=40)
        if result.returncode==0 or 'FAIL' not in result.stderr:raise RuntimeError('Missing row merge negative control escaped detection')
        print('PASS omitted-covered-merge negative control detected')
        if args.benchmark:
            env=dict(os.environ,TTT2_RENDERLOG='0',TTT2_PRIMLOG='0');env.pop('TTT2_MAXPIX',None)
            for name,exe in executables.items():
                result=subprocess.run([str(exe),'bench'],capture_output=True,text=True,env=env,timeout=60)
                if result.returncode:raise RuntimeError(result.stdout+result.stderr)
                print(name+': '+result.stdout.strip())

if __name__=='__main__':main()
