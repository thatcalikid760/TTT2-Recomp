"""Check production ARGB conversion bit-for-bit against scalar std::lround.

Includes every UNORM8 rounding boundary, random IEEE754 inputs, NaNs/infinities,
all MXCSR rounding modes, unaligned float4 storage, and mutation canaries.
The naive '+0.5 then truncate' shortcut is a required failing negative control.
"""
import argparse
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <emmintrin.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
struct V4 {float c[4]{};float& operator[](int i){return c[i];}float operator[](int i)const{return c[i];}};
@PRODUCTION@
uint32_t Reference(const V4& c){
    auto q=[](float f){return uint32_t(std::lround(std::clamp(f,0.0f,1.0f)*255.0f));};
    return(q(c[3])<<24)|(q(c[0])<<16)|(q(c[1])<<8)|q(c[2]);
}
uint64_t checks=0;
bool Check(const V4& color){
    alignas(16) std::array<uint8_t,32> storage;storage.fill(0xA5);
    auto* at=reinterpret_cast<V4*>(storage.data()+4);std::memcpy(at,&color,16);
    auto original=storage;uint32_t expected=Reference(color),actual=PackArgb(*at);++checks;
    if(actual!=expected||storage!=original){
        uint32_t u[4];std::memcpy(u,color.c,16);
        std::fprintf(stderr,"FAIL pack %08X %08X %08X %08X mode=%X got=%08X expected=%08X\n",u[0],u[1],u[2],u[3],_mm_getcsr(),actual,expected);return false;
    }
    return true;
}
int main(){
    unsigned saved=_mm_getcsr();uint32_t random=0x8D12EFA3;
    for(unsigned mode:{_MM_ROUND_NEAREST,_MM_ROUND_DOWN,_MM_ROUND_UP,_MM_ROUND_TOWARD_ZERO}){
        _mm_setcsr((saved&~_MM_ROUND_MASK)|mode);
        // Color lane permutations also expose RGBA/ARGB byte-order mistakes.
        for(unsigned lane=0;lane<4;++lane)for(unsigned step=0;step<256;++step){
            float boundary=(float(step)+.5f)/255.0f;
            for(int neighbor=-8;neighbor<=8;++neighbor){
                float value=boundary;
                for(int i=0;i<std::abs(neighbor);++i)value=std::nextafter(value,neighbor<0?-INFINITY:INFINITY);
                V4 c{{.25f,.5f,.75f,1}};c[lane]=value;if(!Check(c))return 1;
            }
        }
        for(uint32_t bits:{0u,0x80000000u,1u,0x80000001u,0x007FFFFFu,0x00800000u,
                0x3F800000u,0x3F800001u,0xBF800000u,0x7F7FFFFFu,0xFF7FFFFFu,
                0x7F800000u,0xFF800000u,0x7FC00000u,0xFFC00000u,0x7F800001u,0xFFFFFFFFu}){
            float value;std::memcpy(&value,&bits,4);
            for(int lane=0;lane<4;++lane){V4 c{{.1f,.2f,.3f,.4f}};c[lane]=value;if(!Check(c))return 1;}
        }
        for(unsigned i=0;i<500000;++i){
            V4 c;for(float& f:c.c){random^=random<<13;random^=random>>17;random^=random<<5;std::memcpy(&f,&random,4);}
            if(!Check(c))return 1;
        }
    }
    _mm_setcsr(saved);
    std::printf("PASS %llu ARGB packing checks: all boundaries/rounding modes, IEEE754 edge cases, unaligned loads, unchanged input\n",(unsigned long long)checks);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'port/src/runtime/kernel/gpu_render.cpp')
    parser.add_argument('--benchmark', action='store_true', help='Compare production and scalar packing in a synthetic1280x720 draw')
    args = parser.parse_args()
    source = args.source.read_text(encoding='utf-8-sig')
    begin = source.index('uint32_t PackArgb(')
    end = source.index('\n}', begin) + 2
    production = source[begin:end]
    with tempfile.TemporaryDirectory(prefix='ttt2-pixel-pack-') as temp:
        folder = Path(temp)
        executable = compile_one(folder, 'pixel_pack', HARNESS.replace('@PRODUCTION@', production), [])
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        print(result.stdout, end='')
        wrong = '''uint32_t PackArgb(const V4& c){
            auto q=[](float f){float x=std::clamp(f,0.0f,1.0f)*255.0f;return std::isnan(x)?uint32_t(std::lround(x)):uint32_t(x+.5f);};
            return(q(c[3])<<24)|(q(c[0])<<16)|(q(c[1])<<8)|q(c[2]);
        }'''
        executable = compile_one(folder, 'pixel_pack_bad', HARNESS.replace('@PRODUCTION@', wrong), [])
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
        if result.returncode == 0 or 'FAIL pack' not in result.stderr:
            raise RuntimeError('Naive rounding negative control escaped detection: '+result.stdout+result.stderr)
        print('PASS negative control: adding 0.5 before conversion changes a boundary pixel')
        if args.benchmark:
            from test_raster_counters import ADAPTER, MAIN, KERNEL
            (folder/'kernel_shared.h').write_text(ADAPTER)
            (folder/'gpu_render.h').write_text((KERNEL/'gpu_render.h').read_text())
            scalar_begin = HARNESS.index('uint32_t Reference(')
            scalar_end = HARNESS.index('\n}', scalar_begin) + 2
            scalar = HARNESS[scalar_begin:scalar_end].replace('Reference(', 'PackArgb(')
            legacy = source[:begin] + scalar + source[end:]
            executables = {name:compile_one(folder, name, code + MAIN, [folder])
                           for name,code in [('production',source),('scalar',legacy)]}
            timings = {name:[] for name in executables}
            env = dict(os.environ, TTT2_RENDERLOG='0', TTT2_PRIMLOG='0')
            env.pop('TTT2_MAXPIX', None)
            env.pop('TTT2_TEXDUMP', None)
            for repeat in range(5):
                order = ['scalar','production'] if repeat % 2 == 0 else ['production','scalar']
                for name in order:
                    result = subprocess.run([str(executables[name]),'bench'],capture_output=True,
                                            text=True,env=env,timeout=30)
                    match = re.search(r'BENCH ([0-9.]+) ms', result.stdout)
                    if result.returncode or not match:
                        raise RuntimeError(result.stdout + result.stderr)
                    timings[name].append(float(match[1]))
            for name,values in timings.items():
                print(f'BENCH {name}: median {statistics.median(values):.3f}ms per synthetic1280x720 draw; samples={values}')


if __name__ == '__main__':
    main()
