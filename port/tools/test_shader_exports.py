"""Validate Xenos ALU export masks using the actual software shader interpreter.

Reference: Xenia gpu/ucode.h AluInstruction GetVectorOpResultWriteMask,
GetScalarOpResultWriteMask, GetConstant0WriteMask and GetConstant1WriteMask.
No game instructions are embedded. --capture optionally replays locally captured
YUV pixel microcode and plane data; the normal test uses synthetic instructions.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one
from test_raster_counters import ADAPTER, KERNEL

MAIN = r'''
unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
 uint32_t regs[0x8000]{};
 const V4 initial{{-.9f,-.8f,-.7f,-.6f}}, vectorValue{{-.25f,.25f,1.5f,.75f}};
 for(unsigned vm=0;vm<16;++vm)for(unsigned sm=0;sm<16;++sm)
 for(unsigned zero=0;zero<2;++zero)for(unsigned clamp=0;clamp<4;++clamp)
 for(unsigned dest:{0u,5u,62u})for(unsigned retain=0;retain<2;++retain){
  ShaderCtx sc;sc.regs=regs;sc.r[1]=vectorValue;sc.r[2]=V4{{.25f,.5f,.75f,2}};sc.ps=.625f;
  sc.exports[dest]=initial;sc.exports[13]=V4{{42,42,42,42}};
  Alu in{dest|(13<<8)|(zero<<14)|0x8000|(vm<<16)|(sm<<20)|((clamp&1)<<24)|((clamp>>1)<<25)|((retain?S_RETAIN_PREV:S_MAXS)<<26),0,
         0xE0000000u|(V_MAX<<24)|(1<<16)|(1<<8)|2};
  ExecAlu(sc,in);
  for(unsigned c=0;c<4;++c){
   bool v=vm&(1<<c),s=sm&(1<<c);float expected=initial[c];
   if(v&&s)expected=1;
   else if(v)expected=clamp&1?std::clamp(vectorValue[c],0.f,1.f):vectorValue[c];
   else if(s){expected=retain?.625f:2.f;if(clamp&2)expected=std::clamp(expected,0.f,1.f);}
   else if(zero)expected=0;
   CHECK(sc.exports[dest][c]==expected);CHECK(sc.exports[13][c]==42);
  }
  CHECK(sc.ps==(retain?.625f:2.f));
  bool written=(vm|sm|(zero?15:0))!=0;
  CHECK(sc.exportMask==((written&&dest<16)?(1u<<dest):0));CHECK(sc.wrotePos==(written&&dest==62));
 }
 // Predicate rejection must suppress constant writes as well as both ALU units.
 for(unsigned vm=0;vm<16;++vm)for(unsigned sm=0;sm<16;++sm){
  ShaderCtx sc;sc.regs=regs;sc.p0=true;sc.exports[0]=initial;sc.ps=.625f;
  Alu in{0xC800C000u|(vm<<16)|(sm<<20),1u<<28,0xE2000000};ExecAlu(sc,in);
  CHECK(!std::memcmp(&sc.exports[0],&initial,16)&&sc.exportMask==0&&sc.ps==.625f);
 }
 // RetainPrev is still a write operation; non-export destinations remain separate.
 ShaderCtx sc;sc.regs=regs;sc.r[1]=vectorValue;sc.ps=.625f;
 Alu temp{5u|(7u<<8)|(1u<<16)|(8u<<20)|(uint32_t(S_RETAIN_PREV)<<26),0,0xE2000000u|(1<<16)|(1<<8)};
 ExecAlu(sc,temp);CHECK(sc.r[5][0]==-.25f&&sc.r[7][3]==.625f&&sc.exportMask==0);
 std::printf("PASS %u shader export checks: all mask combinations, constant0/1, shared export destination, clamps, RetainPrev, predicates\n",checks);
}
'''

REPLAY = r'''
#include <fstream>
std::vector<uint8_t> Read(const char* path){std::ifstream f(path,std::ios::binary);return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),{});}
int main(int argc,char**argv){
 g_base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_READWRITE));if(!g_base)return 2;
 uint32_t regs[0x8000];auto raw=Read(argv[1]);if(raw.size()!=sizeof(regs))return 3;std::memcpy(regs,raw.data(),sizeof(regs));
 raw=Read(argv[2]);std::vector<uint32_t> code(raw.size()/4);for(size_t i=0;i<code.size();++i)code[i]=__builtin_bswap32(reinterpret_cast<uint32_t*>(raw.data())[i]);
 for(int i=3;i+1<argc;i+=2){uint32_t at=uint32_t(std::stoul(argv[i],nullptr,16));auto bytes=Read(argv[i+1]);if(!VirtualAlloc(g_base+at,(bytes.size()+4095)&~size_t(4095),MEM_COMMIT,PAGE_READWRITE))return 4;std::memcpy(g_base+at,bytes.data(),bytes.size());}
 uint64_t rgb=1469598103934665603ull;float amin=INFINITY,amax=-INFINITY;unsigned notOne=0;double sum=0;
 for(unsigned y=0;y<90;++y)for(unsigned x=0;x<160;++x){
  ShaderCtx sc;sc.regs=regs;sc.pixel=true;sc.r[0][0]=(x+.5f)/160;sc.r[0][1]=(y+.5f)/90;
  RunShader(sc,code);const V4& out=sc.exports[0];
  for(int c=0;c<3;++c){uint32_t bits;std::memcpy(&bits,&out.c[c],4);rgb=(rgb^bits)*1099511628211ull;}
  amin=std::min(amin,out[3]);amax=std::max(amax,out[3]);notOne+=out[3]!=1;sum+=out[3];
 }
 std::printf("RGB=%016llX alpha_min=%g alpha_max=%g alpha_mean=%g alpha_not_one=%u\n",(unsigned long long)rgb,amin,amax,sum/14400,notOne);
}
'''


def legacy_exports(source):
    source = source.replace('    if (in.exportData())\n        mask &= ~(scalarUnit ? in.vectorWriteMask() : in.scalarWriteMask());\n', '')
    source = source.replace('scalarUnit && !in.exportData() ? in.scalarDest() : in.vectorDest()', 'scalarUnit ? in.scalarDest() : in.vectorDest()')
    source = source.replace('    WriteExportConstants(sc, in);\n', '')
    begin = source.index('    if (sop == S_RETAIN_PREV)\n')
    end = source.index('    float s0, s1;', begin)
    return source[:begin] + '    if (sop == S_RETAIN_PREV) return;\n' + source[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path)
    args = parser.parse_args()
    source = (KERNEL/'gpu_render.cpp').read_text()
    old = legacy_exports(source)
    with tempfile.TemporaryDirectory(prefix='ttt2-shader-export-') as temp:
        folder = Path(temp)
        (folder/'kernel_shared.h').write_text(ADAPTER)
        (folder/'gpu_render.h').write_text((KERNEL/'gpu_render.h').read_text())
        exe = compile_one(folder,'exports',source+MAIN,[folder])
        result = subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
        if result.returncode:
            raise RuntimeError(result.stdout+result.stderr)
        print(result.stdout,end='')
        exe = compile_one(folder,'exports_old',old+MAIN,[folder])
        result = subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
        if result.returncode == 0 or 'FAIL' not in result.stderr:
            raise RuntimeError('Original export-mask implementation escaped detection: '+result.stdout+result.stderr)
        print('PASS original export-mask/RetainPrev negative control')
        if args.capture:
            # Real guest addresses are preserved in a reserved sparse4GiB range.
            adapter = ADAPTER.replace('uint8_t guest[0x20000]{};\nuint8_t* g_base = guest;', 'uint8_t* g_base;')
            start = adapter.index('uint32_t GuestOffset(')
            end = adapter.index('\nuint8_t GRead8',start)
            adapter = adapter[:start] + 'uint32_t GuestOffset(uint32_t address){return address;}\n' + adapter[end:]
            (folder/'kernel_shared.h').write_text(adapter)
            capture = json.loads((args.capture/'capture.json').read_text())
            params = [str((args.capture/'regs.bin').resolve()),str((args.capture/'ps.bin').resolve())]
            for texture in capture['textures']:
                params += [texture['base'],str((args.capture/texture['path']).resolve())]
            outputs = {}
            for name,code in [('production',source),('old',old)]:
                exe = compile_one(folder,'replay_'+name,code+REPLAY,[folder])
                result = subprocess.run([str(exe)]+params,capture_output=True,text=True,timeout=20,
                                        env=dict(os.environ,TTT2_RENDERLOG='0',TTT2_PRIMLOG='0'))
                if result.returncode:
                    raise RuntimeError(result.stdout+result.stderr)
                outputs[name] = result.stdout.strip()
                print('CAPTURE '+name+': '+outputs[name])
            if outputs['production'].split()[0] != outputs['old'].split()[0] or 'alpha_not_one=0' not in outputs['production']:
                raise RuntimeError('YUV replay changed RGB or failed to force alpha1: '+repr(outputs))
            print('PASS local captured YUV shader: RGB bit-identical, output alpha exactly1')


if __name__ == '__main__':
    main()

