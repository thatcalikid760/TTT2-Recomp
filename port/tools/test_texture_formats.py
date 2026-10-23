"""Component order of every packed texture format the production sampler decodes.

The Xenos fetch numbers the components of a packed texel from the LOW bits (x = lowest field):
a k_8_8_8_8 render target and texture are bitwise equivalent, and Xenia's host formats load
k_5_6_5 / k_1_5_5_5 / k_4_4_4_4 / k_6_5_5 with the guest's lowest field moved into host red
(R5G6B5ToB5G6R5, R5G5B5A1ToB5G5R5A1, RGBA4ToBGRA4, R5G5B6ToB5G6R5WithRBGASwizzle) and k_2_10_10_10
/ k_8_8 / k_16_16 as R10G10B10A2 / R8G8 / R16G16 copies. D3DFMT_A8R8G8B8 data (0xAARRGGBB words)
with its fetch swizzle .zyxw (0x60A) must therefore sample as RGBA. Reading these formats high
field first drew TTT2's red recoverable-health bar blue (2026-10-12).

References: tools/ref_xenia/d3d12_texture_cache.h (host_formats_), xenos.h
IsColorResolveFormatBitwiseEquivalent. Synthetic texels only; negative controls restore the
old orders and must fail.

    python port/tools/test_texture_formats.py
"""
import subprocess
import tempfile
from pathlib import Path

from test_raster_counters import ADAPTER, KERNEL
from test_vcmpgtuh import compile_one

MAIN = r'''
unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
void BE32(uint32_t at,uint32_t v){for(int i=0;i<4;++i)guest[at+i]=uint8_t(v>>(24-8*i));}
void BE16(uint32_t at,uint16_t v){guest[at]=uint8_t(v>>8);guest[at+1]=uint8_t(v);}
bool Near(const V4& got,float x,float y,float z,float w){const float e[4]={x,y,z,w};
    for(int c=0;c<4;++c)if(std::fabs(got[c]-e[c])>1e-5f){std::fprintf(stderr,"  got %f %f %f %f want %f %f %f %f\n",got[0],got[1],got[2],got[3],x,y,z,w);return false;}return true;}
V4 Fetch(uint32_t format,uint32_t swizzle){Texture t;t.valid=true;t.base=0x1000;t.width=t.height=1;t.pitch=32;
    t.format=format;t.tiled=false;t.swizzle=swizzle;t.endian=2;return SampleTexel(t,0,0);}
int main(){
    const uint32_t XYZW=0x688, ARGB=0x60A;      // identity; D3DFMT_A8R8G8B8's .zyxw
    // 8_8_8_8: x = bits 0-7.
    BE32(0x1000,0x11223344u);
    CHECK(Near(Fetch(6,XYZW),0x44/255.f,0x33/255.f,0x22/255.f,0x11/255.f));
    // A8R8G8B8 texel (A=FF R=C0 G=40 B=10) through its swizzle: RGBA.
    BE32(0x1000,0xFFC04010u);
    CHECK(Near(Fetch(6,ARGB),0xC0/255.f,0x40/255.f,0x10/255.f,1.f));
    // 2_10_10_10: x = bits 0-9, w = bits 30-31.
    BE32(0x1000,(2u<<30)|(700u<<20)|(300u<<10)|100u);
    CHECK(Near(Fetch(7,XYZW),100/1023.f,300/1023.f,700/1023.f,2/3.f));
    // 5_6_5: x = bits 0-4, y = 5-10, z = 11-15.
    BE16(0x1000,uint16_t((3u<<11)|(40u<<5)|17u));
    CHECK(Near(Fetch(4,XYZW),17/31.f,40/63.f,3/31.f,1.f));
    // 6_5_5: x = bits 0-4, y = 5-9, z = 10-15.
    BE16(0x1000,uint16_t((50u<<10)|(9u<<5)|21u));
    CHECK(Near(Fetch(5,XYZW),21/31.f,9/31.f,50/63.f,1.f));
    // 1_5_5_5: x = bits 0-4, y = 5-9, z = 10-14, w = bit 15.
    BE16(0x1000,uint16_t((1u<<15)|(6u<<10)|(25u<<5)|11u));
    CHECK(Near(Fetch(3,XYZW),11/31.f,25/31.f,6/31.f,1.f));
    // 4_4_4_4: x = bits 0-3 ... w = bits 12-15.
    BE16(0x1000,uint16_t((9u<<12)|(5u<<8)|(12u<<4)|2u));
    CHECK(Near(Fetch(15,XYZW),2/15.f,12/15.f,5/15.f,9/15.f));
    // 8_8: x = low byte.
    BE16(0x1000,uint16_t(0xA05Fu));
    CHECK(Near(Fetch(10,XYZW),0x5F/255.f,0xA0/255.f,0.f,1.f));
    // 16_16: x = low half.
    BE32(0x1000,0x80004000u);
    CHECK(Near(Fetch(25,XYZW),0x4000/65535.f,0x8000/65535.f,0.f,1.f));
    // Vertex fetch of 8_8_8_8 and 2_10_10_10 (same low-bits rule).
    BE32(0x1000,0x11223344u);
    CHECK(Near(UnpackFormat(0x1000,6,false,false,true),0x44/255.f,0x33/255.f,0x22/255.f,0x11/255.f));
    BE32(0x1000,(1u<<30)|(5u<<20)|(6u<<10)|7u);
    CHECK(Near(UnpackFormat(0x1000,7,false,false,true),7/1023.f,6/1023.f,5/1023.f,1/3.f));
    std::printf("PASS %u packed texture/vertex format component-order checks\n",checks);
    return 0;
}
'''


def replace_once(s, old, new):
    if s.count(old) != 1:
        raise RuntimeError('missing source anchor ' + old[:70])
    return s.replace(old, new, 1)


def run(folder, name, source):
    exe = compile_one(folder, name, source + MAIN, [folder])
    return subprocess.run([str(exe)], capture_output=True, text=True, timeout=45)


def main():
    source = (KERNEL / 'gpu_render.cpp').read_text()
    with tempfile.TemporaryDirectory(prefix='ttt2-texfmt-') as temporary:
        folder = Path(temporary)
        (folder / 'kernel_shared.h').write_text(ADAPTER)
        (folder / 'gpu_render.h').write_text((KERNEL / 'gpu_render.h').read_text())
        result = run(folder, 'formats', source)
        print(result.stdout + result.stderr, end='')
        if result.returncode:
            raise SystemExit(result.returncode)
        negatives = [
            ('argb_8888', replace_once(source,
                '        v[0] = norm8(u & 0xFF);\n        v[1] = norm8((u >> 8) & 0xFF);\n        v[2] = norm8((u >> 16) & 0xFF);',
                '        v[0] = norm8((u >> 16) & 0xFF);\n        v[1] = norm8((u >> 8) & 0xFF);\n        v[2] = norm8(u & 0xFF);')),
            ('high_first_565', replace_once(source,
                'v[0] = ((vertexFetch ? u >> 11 : u) & 0x1F) / 31.0f;', 'v[0] = ((u >> 11) & 0x1F) / 31.0f;')),
            ('high_first_4444', replace_once(source,
                '            v[0] = (u & 0xF) / 15.0f;\n            v[1] = ((u >> 4) & 0xF) / 15.0f;\n            v[2] = ((u >> 8) & 0xF) / 15.0f;',
                '            v[0] = ((u >> 8) & 0xF) / 15.0f;\n            v[1] = ((u >> 4) & 0xF) / 15.0f;\n            v[2] = (u & 0xF) / 15.0f;')),
        ]
        for name, bad in negatives:
            result = run(folder, name, bad)
            if result.returncode == 0 or 'FAIL' not in result.stderr:
                raise SystemExit('negative control escaped: ' + name)
            print('PASS negative control: ' + name)


if __name__ == '__main__':
    main()
