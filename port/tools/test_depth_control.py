"""Exercise the production rasterizer's depth enable/write-enable interaction.

Primary reference: Xenia src/xenia/gpu/d3d12/pipeline_cache.cc, depth_write
assignment inside normalized_depth_control.z_enable (lines 1441-1447).
Tests use synthetic shaders/vertices only; no game instructions or assets.
"""
import os
from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one
from test_raster_counters import ADAPTER, KERNEL, MAIN, checked_replace

CASES = r'''
int main() {
 constexpr unsigned width=16,height=12,count=width*height;
 constexpr uint32_t background=0xFF112233,color=0x804080BF;
 // An inactive depth test must not allocate a depth surface just because the
 // stored write-enable bit is set, including a draw with no color writes.
 for(unsigned colorPass=0;colorPass<2;++colorPass) {
  Configure(width,height);regs[0x2200]=4;regs[0x2104]=colorPass?15:0;
  g_depth.clear();GpuFillEdram(0,width,height,background);
  bool drawn=GpuExecuteDraw(regs,(6<<16)|(2<<6)|4,0,false,{0x100,9},{0x200,6});
  CHECK(g_depth.empty());CHECK(drawn==bool(colorPass));
 }
 // Cover every enable/write/function combination with a preexisting depth
 // surface. Inactive depth must preserve its contents, not merely avoid an
 // allocation. Color/depth-only paths and alpha rejection are both exercised.
 auto& depth=GetDepth(0);
 for(unsigned enabled=0;enabled<2;++enabled)for(unsigned write=0;write<2;++write)
 for(unsigned func=0;func<8;++func)for(unsigned colorPass=0;colorPass<2;++colorPass)
 for(unsigned rejectAlpha=0;rejectAlpha<2;++rejectAlpha) {
  Configure(width,height);regs[0x2200]=(enabled<<1)|(write<<2)|(func<<4);
  regs[0x2104]=colorPass?15:0;
  if(rejectAlpha){regs[0x2202]=8|4;regs[0x210E]=Bits(.75f);}
  std::fill(depth.begin(),depth.begin()+count+1,.5f);
  GpuFillEdram(0,width,height,background);
  bool drawn=GpuExecuteDraw(regs,(6<<16)|(2<<6)|4,0,false,{0x100,9},{0x200,6});
  bool depthPass=!enabled || func==1 || func==3 || func==5 || func==7;
  bool fragmentPass=depthPass && !rejectAlpha;
  CHECK(drawn==bool(colorPass && fragmentPass));
  const auto& pixels=GetSurface(0).px;
  for(unsigned i=0;i<count;++i) {
   CHECK(depth[i]==(enabled && write && fragmentPass?.25f:.5f));
   CHECK(pixels[i]==(colorPass && fragmentPass?color:background));
  }
  CHECK(depth[count]==.5f); // No writes beyond the configured draw/scissor.
 }
 std::printf("PASS %u depth-control checks\n",checks);
}
'''


def main():
    source = (KERNEL / 'gpu_render.cpp').read_text()
    # Reuse only synthetic fixture setup, not the row-counter instrumentation.
    fixture = MAIN[:MAIN.index('int main(')] + CASES
    old = checked_replace(source,
        'zWrite = zEnable && ((depthCtl >> 2) & 1) != 0;',
        'zWrite = ((depthCtl >> 2) & 1) != 0;')
    with tempfile.TemporaryDirectory(prefix='ttt2-depth-control-') as temp:
        folder = Path(temp)
        (folder / 'kernel_shared.h').write_text(ADAPTER)
        (folder / 'gpu_render.h').write_text((KERNEL / 'gpu_render.h').read_text())
        env = dict(os.environ, TTT2_RENDERLOG='0', TTT2_PRIMLOG='0')
        env.pop('TTT2_MAXPIX', None)
        for name, code in [('production', source), ('old_ungated_write', old)]:
            exe = compile_one(folder, name, code + fixture, [folder])
            result = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=30)
            if name == 'production':
                if result.returncode:
                    raise RuntimeError(result.stdout + result.stderr)
                print(result.stdout, end='')
            elif not result.returncode or 'FAIL' not in result.stderr:
                raise RuntimeError('Ungated depth-write negative control escaped detection')
            else:
                print('PASS ungated depth-write negative control')


if __name__ == '__main__':
    main()
