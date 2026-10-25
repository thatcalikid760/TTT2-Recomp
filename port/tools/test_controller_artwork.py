"""Controller Setup artwork: isolated atlas edits, device styles, and rebinds.

--atlas accepts an optional locally captured 512x512 tiled BC3 texture, never
checked into the repo. --capture writes generated artwork previews only.
"""
import argparse
import os
from pathlib import Path
import subprocess

from test_vcmpgtuh import compile_one

KERNEL = Path(__file__).resolve().parents[1] / 'src/runtime/kernel'
CHECKER = r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "button_prompts.cpp"
#include "keyboard_keys.cpp"
#include <fstream>
using namespace button_prompts;
unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(int argc, char** argv) {
    constexpr int W=512, H=512, P=W*4+16;
    std::vector<uint8_t> original(P*H);
    for (size_t i=0;i<original.size();++i) original[i]=uint8_t(i*37+(i>>9));
    auto xbox=original;
    PaintController(Style::Xbox,xbox.data(),W,H,P);
    CHECK(xbox==original);
    std::vector<uint8_t> styles[3];
    for (int s=1;s<4;++s) {
        auto& a=styles[s-1]; a=original;
        PaintController(Style(s),a.data(),W,H,P);
        size_t inside=0,outside=0,opaque=0,transparent=0;
        for (int y=0;y<H;++y) for (int x=0;x<P;++x) {
            const bool rect=y>=220 && y<416 && x<316*4;
            if (a[y*P+x]!=original[y*P+x]) (rect?inside:outside)++;
            if(rect && x%4==3) { opaque+=a[y*P+x]>200; transparent+=a[y*P+x]==0; }
        }
        CHECK(inside>20000 && outside==0);
        CHECK(opaque>10000 && transparent>5000);
        auto again=original; PaintController(Style(s),again.data(),W,H,P);
        CHECK(again==a);
        std::ofstream out(std::string(argv[1])+"/controller-style-"+std::to_string(s)+".rgba",std::ios::binary);
        for(int y=220;y<416;++y) out.write(reinterpret_cast<const char*>(a.data()+y*P),316*4);
    }
    CHECK(styles[0]!=styles[1] && styles[1]!=styles[2] && styles[0]!=styles[2]);
    NoteInput(Device::PlayStation4); auto generation=Generation();
    CHECK(Current()==Style::PlayStation4);
    NoteInput(Device::PlayStation5); CHECK(Current()==Style::PlayStation5 && Generation()!=generation);
    NoteInput(Device::Keyboard); generation=Generation();
    CHECK(Current()==Style::Keyboard);
    const int previousKey=keyboard_keys::Key(keyboard_keys::A);
    keyboard_keys::Assign(keyboard_keys::A,'J');
    CHECK(Generation()!=generation);
    auto rebound=original; PaintController(Current(),rebound.data(),W,H,P);
    CHECK(rebound!=styles[2]);
    keyboard_keys::Assign(keyboard_keys::A,previousKey);
    NoteInput(Device::Xbox); CHECK(Current()==Style::Xbox);
    auto invalid=original;
    PaintController(Style::PlayStation4,invalid.data(),W-1,H,P);
    PaintController(Style::PlayStation5,invalid.data(),W,H-1,P);
    PaintController(Style::Keyboard,invalid.data(),W,H,W*4-1);
    CHECK(invalid==original);
    PaintController(Style::Keyboard,nullptr,W,H,P);
    std::vector<uint8_t> raw(W*H,0);
    CHECK(!IsControllerAtlas(20,W,H,true,raw.data(),raw.size()));
    CHECK(!IsControllerAtlas(20,W,H,true,nullptr,raw.size()));
    if(argc>2) {
        std::ifstream input(argv[2],std::ios::binary);
        raw.assign(std::istreambuf_iterator<char>(input),{});
        CHECK(raw.size()==W*H);
        CHECK(IsControllerAtlas(20,W,H,true,raw.data(),raw.size()));
        CHECK(!IsControllerAtlas(19,W,H,true,raw.data(),raw.size()));
        CHECK(!IsControllerAtlas(20,W-4,H,true,raw.data(),raw.size()));
        CHECK(!IsControllerAtlas(20,W,H-4,true,raw.data(),raw.size()));
        CHECK(!IsControllerAtlas(20,W,H,false,raw.data(),raw.size()));
        CHECK(!IsControllerAtlas(20,W,H,true,raw.data(),raw.size()-1));
        raw[12345]^=1;
        CHECK(!IsControllerAtlas(20,W,H,true,raw.data(),raw.size()));
    }
    std::printf("PASS %u controller-artwork checks\n",checks);
}
'''


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work-dir',type=Path,required=True)
    ap.add_argument('--atlas',type=Path)
    ap.add_argument('--capture',action='store_true')
    ap.add_argument('--negative-control',action='store_true')
    args=ap.parse_args()
    code=CHECKER
    if args.negative_control:
        source=(KERNEL/'button_prompts.cpp').read_text(encoding='utf-8-sig')
        old='if (style == Style::Xbox || !rgba || width != 512 || height != 512 || pitch < width * 4) return;'
        assert source.count(old)==1
        source=source.replace(old,old.replace('style == Style::Xbox || ',''))
        code=code.replace('#include "button_prompts.cpp"',source)
    exe=compile_one(args.work_dir,'controller_artwork',code,[KERNEL],'/link user32.lib')
    ini=args.work_dir/'controller-artwork-test.ini'
    ini.write_text('[Input]\nButtonPrompts=Auto\n',encoding='utf-8')
    result=subprocess.run([str(exe),str(args.work_dir)]+([str(args.atlas)] if args.atlas else []),
                          env=dict(os.environ,TTT2_SETTINGS_INI=str(ini)),capture_output=True,text=True,timeout=60)
    print(result.stdout+result.stderr,end='')
    if args.negative_control:
        assert result.returncode!=0 and 'xbox==original' in result.stderr
        print('PASS negative control: Xbox preservation regression detected')
        return
    if result.returncode: raise SystemExit(result.returncode)
    if args.capture:
        from PIL import Image
        for s in range(1,4):
            raw=args.work_dir/f'controller-style-{s}.rgba'
            Image.frombytes('RGBA',(316,196),raw.read_bytes()).save(raw.with_suffix('.png'))


if __name__=='__main__':
    main()
