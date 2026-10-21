"""Check the real bounded HLSL translator's rejection and export policies.

Synthetic microcode only. The companion test_d3d11_replay.py verifies actual
captured draw pixels on hardware; this test needs no graphics device.
"""
import subprocess
import tempfile
from pathlib import Path

from test_vcmpgtuh import compile_one
from test_raster_counters import ADAPTER, KERNEL

MAIN = r'''
#include "gpu_shader_subset.h"
unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
uint32_t regs[0x8000]{};
bool Rejected(const std::vector<uint32_t>& code) {
 try{ttt2_d3d11::Translate(code,regs,true);return false;}
 catch(const std::runtime_error&){return true;}
}
int main(){
 using ttt2_d3d11::Translate;
 // One complete EXEC with MAX c0,c0 -> color0; RetainPrev does not write.
 std::vector<uint32_t> base={1|(1<<12),2<<12,0,(50u<<26)|0xF8000,0,2u<<24};
 CHECK(Translate(base,regs,true).operations==1);
 for(unsigned vm=0;vm<16;++vm)for(unsigned sm=0;sm<16;++sm)for(unsigned zero=0;zero<2;++zero) {
  auto code=base;code[3]=(uint32_t(S_SUBS)<<26)|0x8000|(vm<<16)|(sm<<20)|(zero<<14);
  bool hasOutput=(vm|sm|zero)!=0;
  if(!hasOutput){CHECK(Rejected(code));continue;}
  auto out=Translate(code,regs,true);
  for(unsigned c=0;c<4;++c) {
   bool v=vm&(1<<c),s=sm&(1<<c);std::string prefix=std::string("o.")+"xyzw"[c]+"=";
   std::string expected=v&&s?"1.0;":v?std::string("v.")+"xyzw"[c]+";":s?"s;":zero?"0.0;":"";
   CHECK(expected.empty()?out.hlsl.find(prefix)==std::string::npos:out.hlsl.find(prefix+expected)!=std::string::npos);
  }
 }
 // Unsupported instruction classes must reject before HLSL/device execution.
 CHECK(Rejected({}));auto code=base;code.pop_back();CHECK(Rejected(code));
 code=base;code[1]=3<<12;CHECK(Rejected(code)); // conditional CF
 code=base;code[0]=(code[0]&~0xFFFu)|100;CHECK(Rejected(code)); // out-of-range EXEC
 code=base;code[3]|=1<<6;CHECK(Rejected(code)); // relative destination
 code=base;code[3]=(code[3]&~0x3Fu)|1;CHECK(Rejected(code)); // other export target
 for(unsigned bit:{28u,29u,30u,31u}){code=base;code[4]|=1u<<bit;CHECK(Rejected(code));}
 code=base;code[5]=(V_DP4<<24);CHECK(Rejected(code)); // unsupported vector opcode
 code=base;code[3]=(code[3]&0x03FFFFFFu)|(S_RCP<<26);CHECK(Rejected(code));
 code=base;code[3]=(code[3]&~(15u<<16))|(1u<<20);CHECK(Rejected(code)); // RetainPrev write
 code=base;code[5]=0xC2000000u|(64u<<16)|(64u<<8);CHECK(Rejected(code));
 // Scalar/vector coissue alias: rejection stays conservative until the CPU
 // oracle's source snapshot timing is independently corrected.
 code={1|(2<<12),2<<12,0,(uint32_t(S_SUBS)<<26)|(1<<16)|(2<<20),0,0xE2000000,
       (50u<<26)|0xF8000,0,2u<<24};CHECK(Rejected(code));
 // Plain 2D texture followed by a complete export.
 std::vector<uint32_t> tex={1|(2<<12)|(1<<16),2<<12,0,1,0x1688,0x4000,
                           (50u<<26)|0xF8000,0,0xC2000000};
 regs[0x4800]=2|(1<<22);regs[0x4801]=0x1000|2;regs[0x4802]=(3<<13)|3;
 regs[0x4803]=0xA00<<1;
 auto translated=Translate(tex,regs,true);CHECK(translated.textures==std::set<unsigned>{0});
 CHECK(translated.hlsl.find("float4 expanded=float4(f.r,f.r,f.r,1)")!=std::string::npos);
 for(unsigned bit:{11u,18u}){code=tex;code[3]|=1u<<bit;CHECK(Rejected(code));}
 for(unsigned bit:{30u,31u}){code=tex;code[4]|=1u<<bit;CHECK(Rejected(code));}
 code=tex;code[5]=0;CHECK(Rejected(code)); // non-2D fetch
 code=tex;code[4]=(code[4]&~0x3000u)|0x2000;CHECK(Rejected(code)); // reserved filter
 code=tex;code[4]=(code[4]&~7u)|6;CHECK(Rejected(code)); // reserved destination swizzle
 // Shader text is the cache identity: baked format/swizzle and alpha policy
 // must invalidate it, while per-draw constants must not require recompilation.
 const auto original=translated.hlsl;
 regs[0x4400]=0x3F800000;CHECK(Translate(tex,regs,true).hlsl==original);
 regs[0x4803]=0;CHECK(Translate(tex,regs,true).hlsl!=original);
 regs[0x4803]=0xA00<<1;regs[0x2202]=8|5;
 const auto alpha=Translate(tex,regs,true).hlsl;CHECK(alpha!=original);
 regs[0x210E]=0x3F000000;CHECK(Translate(tex,regs,true).hlsl==alpha);
 CHECK(Translate(tex,regs,false).hlsl!=alpha);
 std::printf("PASS %u shared shader-subset guard/cache/export checks\n",checks);
}
'''


def main():
    source = (KERNEL / 'gpu_render.cpp').read_text()
    with tempfile.TemporaryDirectory(prefix='ttt2-shader-subset-') as temp:
        folder = Path(temp)
        (folder / 'kernel_shared.h').write_text(ADAPTER)
        (folder / 'gpu_render.h').write_text((KERNEL / 'gpu_render.h').read_text())
        exe = compile_one(folder, 'subset', source + MAIN, [folder, KERNEL])
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        print(result.stdout, end='')
        header = (KERNEL / 'gpu_shader_subset.h').read_text()
        anchor = ' && !a.isPredicated()'
        if header.count(anchor) != 1:
            raise RuntimeError('Predicate guard anchor changed')
        (folder / 'gpu_shader_subset.h').write_text(header.replace(anchor, '', 1))
        exe = compile_one(folder, 'missing_predicate_guard', source + MAIN, [folder, KERNEL])
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        if not result.returncode or 'FAIL' not in result.stderr:
            raise RuntimeError('Missing predicate guard negative control escaped detection')
        print('PASS omitted predicate-guard negative control')


if __name__ == '__main__':
    main()
