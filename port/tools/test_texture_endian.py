"""Production BC1/BC2/BC3 sampler tests for all four Xenos byte-order modes.

Uses synthetic blocks only. Optional --capture decodes a previously captured raw
texture through the production sampler for visual comparison, without a game run.
References: Xenia xenos.h Endian, shaders/endian.xesli,
texture_load_dxt5_rgba8.cs.xesl; Microsoft Direct3D block compression specification.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_vcmpgtuh import compile_one
from test_raster_counters import ADAPTER, KERNEL

MAIN = r'''
#include <array>
unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
void LE16(uint8_t* p,uint16_t v) {p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}
void LE32(uint8_t* p,uint32_t v) {for(int i=0;i<4;++i)p[i]=uint8_t(v>>(8*i));}
unsigned SwapMask(unsigned endian) {const unsigned masks[]={0,1,3,2};return masks[endian];}
void Encode(uint32_t at,const std::array<uint8_t,16>& canonical,unsigned bytes,unsigned endian) {
    for(unsigned i=0;i<bytes;++i)guest[at+(i^SwapMask(endian))]=canonical[i];
}
V4 RGB565(uint16_t c) {V4 v;v[0]=float(c>>11)/31;v[1]=float((c>>5)&63)/63;v[2]=float(c&31)/31;v[3]=1;return v;}
bool Equal(const V4& got,const V4& expected) {for(int c=0;c<4;++c)if(std::fabs(got[c]-expected[c])>0.000001f)return false;return true;}
int main(int argc,char** argv) {
    if(argc==9) {
        FILE* input=std::fopen(argv[1],"rb");if(!input)return 2;
        std::fread(guest+0x1000,1,sizeof(guest)-0x1000,input);std::fclose(input);
        Texture t;t.valid=true;t.base=0x1000;t.width=std::strtoul(argv[3],nullptr,0);
        t.height=std::strtoul(argv[4],nullptr,0);t.pitch=std::strtoul(argv[5],nullptr,0);
        t.format=std::strtoul(argv[6],nullptr,0);t.endian=std::strtoul(argv[7],nullptr,0);
        t.tiled=std::strtoul(argv[8],nullptr,0);t.swizzle=0x688;
        FILE* output=std::fopen(argv[2],"wb");if(!output)return 3;
        for(unsigned y=0;y<t.height;++y)for(unsigned x=0;x<t.width;++x) {
            V4 v=SampleTexel(t,x,y);uint8_t bytes[4];
            for(int c=0;c<4;++c)bytes[c]=uint8_t(std::lround(std::clamp(v[c],0.f,1.f)*255));
            std::fwrite(bytes,1,4,output);
        }
        std::fclose(output);return 0;
    }
    uint32_t regs[0x8000]{};
    for(unsigned endian=0;endian<4;++endian) {
        regs[0x4800]=0x82000002;regs[0x4801]=0x01234014|(endian<<6);
        regs[0x4802]=(255<<13)|255;regs[0x4803]=0x688<<1;
        Texture t=DecodeTexture(regs,0);CHECK(t.endian==endian && t.format==20);
        CHECK(t.base==0xA1234000 && t.width==256 && t.height==256 && t.pitch==256 && t.tiled);
        std::array<uint8_t,16> data{};LE32(data.data(),0x12345678);Encode(0x1000,data,4,endian);
        CHECK(TextureWord(0x1000,endian)==0x12345678);
    }
    for(unsigned format:{18u,19u,20u})for(unsigned endian=0;endian<4;++endian)
    for(bool tiled:{false,true})for(unsigned order=0;order<3;++order)for(unsigned alphaOrder=0;alphaOrder<3;++alphaOrder) {
        Texture t;t.valid=true;t.base=0x1000;t.width=136;t.height=132;t.pitch=160;
        t.format=format;t.endian=endian;t.tiled=tiled;t.swizzle=0x688;
        unsigned bytes=format==18?8:16,colorOffset=format==18?0:8;
        uint16_t c0=order==0?0xF800:0x001F,c1=order==1?0xF800:0x001F;
        if(order==2)c0=c1=0x07E0;
        unsigned a0=alphaOrder==0?231:24,a1=alphaOrder==1?231:24;
        if(alphaOrder==2)a0=a1=80;
        std::array<uint8_t,16> block{};
        LE16(block.data()+colorOffset,c0);LE16(block.data()+colorOffset+2,c1);
        uint32_t colors=0;uint64_t alphas=0;uint64_t bc2Alpha=0;
        for(unsigned p=0;p<16;++p) {colors|=((p*3+p/4)&3)<<(p*2);alphas|=uint64_t((p*5+p/4)&7)<<(p*3);bc2Alpha|=uint64_t(p)<<(p*4);}
        LE32(block.data()+colorOffset+4,colors);
        if(format==19)for(int i=0;i<8;++i)block[i]=uint8_t(bc2Alpha>>(8*i));
        if(format==20){block[0]=uint8_t(a0);block[1]=uint8_t(a1);for(int i=0;i<6;++i)block[2+i]=uint8_t(alphas>>(8*i));}
        std::array<V4,4> palette{};palette[0]=RGB565(c0);palette[1]=RGB565(c1);
        bool four=format!=18 || c0>c1;
        for(int c=0;c<3;++c){palette[2][c]=(palette[0][c]+palette[1][c])*.5f;
            if(four){palette[2][c]=(2*palette[0][c]+palette[1][c])/3;palette[3][c]=(palette[0][c]+2*palette[1][c])/3;}}
        palette[2][3]=1;palette[3][3]=four?1:0;
        float alpha[8]={float(a0)/255,float(a1)/255};
        if(a0>a1)for(int s=2;s<8;++s)alpha[s]=float((8-s)*a0+(s-1)*a1)/(7*255);
        else {for(int s=2;s<6;++s)alpha[s]=float((6-s)*a0+(s-1)*a1)/(5*255);alpha[6]=0;alpha[7]=1;}
        for(unsigned by:{0u,1u,8u,31u,32u})for(unsigned bx:{0u,1u,8u,31u,33u}) {
            auto blockIndex=tiled?GpuTiledOffset2D(bx,by,t.pitch/4,bytes==8?3:4):by*(t.pitch/4)+bx;
            Encode(t.base+blockIndex*bytes,block,bytes,endian);
            for(unsigned p=0;p<16;++p){V4 expected=palette[(colors>>(2*p))&3];
                if(format==19)expected[3]=float(p)/15;
                if(format==20)expected[3]=alpha[(alphas>>(3*p))&7];
                CHECK(Equal(SampleTexel(t,bx*4+(p&3),by*4+(p>>2)),expected));}
        }
        // Bilinear filtering must combine the normalized block's texels, including alpha.
        t.width=t.height=4;t.pitch=4;t.tiled=false;Encode(t.base,block,bytes,endian);
        V4 average;for(int p:{0,1,4,5}){V4 e=palette[(colors>>(2*p))&3];if(format==19)e[3]=float(p)/15;if(format==20)e[3]=alpha[(alphas>>(3*p))&7];for(int c=0;c<4;++c)average[c]+=e[c]*.25f;}
        CHECK(Equal(SampleTexture(t,.25f,.25f,true),average));
    }
    std::printf("PASS %u texture endian, BC1/2/3 palette/alpha, tiled/linear and filtering checks\n",checks);
}
'''


def replace_once(s,old,new):
    if s.count(old)!=1:raise RuntimeError('missing source anchor '+old[:70])
    return s.replace(old,new,1)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture',type=Path)
    args=parser.parse_args()
    source=(KERNEL/'gpu_render.cpp').read_text()
    with tempfile.TemporaryDirectory(prefix='ttt2-texture-') as temporary:
        folder=Path(temporary);(folder/'kernel_shared.h').write_text(ADAPTER.replace('guest[0x20000]','guest[0x80000]'))
        (folder/'gpu_render.h').write_text((KERNEL/'gpu_render.h').read_text())
        exe=compile_one(folder,'sampler',source+MAIN,[folder])
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=45)
        print(result.stdout+result.stderr,end='')
        if result.returncode:raise SystemExit(result.returncode)
        for name,bad in [
            ('wrong_endian',replace_once(source,'    t.endian = (c[1] >> 6) & 3;','    t.endian = 2;')),
            ('old_be32_selectors',replace_once(source,'uint32_t bits = TextureWord(addr + colorOff + 4, t.endian);','uint32_t bits = GRead32(g_base, addr + colorOff + 4);')),
            ('bc3_three_colors',replace_once(source,'c0 > c1 || t.format == FMT_DXT2_3 || t.format == FMT_DXT4_5','c0 > c1'))]:
            negative=compile_one(folder,name,bad+MAIN,[folder]);result=subprocess.run([str(negative)],capture_output=True,text=True,timeout=45)
            if result.returncode==0 or 'FAIL' not in result.stderr:raise RuntimeError('negative control escaped: '+name)
            print('PASS negative control: '+name)
        if args.capture:
            import json
            from PIL import Image
            for t in json.loads((args.capture/'textures.json').read_text()):
                if t['format'] not in (18,19,20):continue
                raw=args.capture/f"tex_{t['slot']}_{t['address']}.bin";out=folder/'rgba.bin'
                subprocess.run([str(exe),str(raw),str(out),str(t['width']),str(t['height']),str(t['pitch']),str(t['format']),str(t['endian']),str(t['tiled'])],check=True)
                im=Image.frombytes('RGBA',(t['width'],t['height']),out.read_bytes());path=args.capture/f"atlas_slot{t['slot']}_production.png";im.save(path);print('Production capture:',path)

if __name__=='__main__':main()
