"""Exercise the actual async message-box implementation (kernel_ui.cpp) without a game.

Only the system-UI boundary is replaced: SysUiShowMessageBox records each box and the test answers
it through the box's `done` callback, as the in-game window does from the main thread.
Production validation, UTF-16 -> UTF-8 copies, cancel detection, overlapped/event completion and
notification order execute unchanged; real Windows VM/protection and event APIs validate the
pointer and completion checks.
"""
from pathlib import Path
import re
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "port/src/runtime"
PREFIX = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
union Register { uint64_t u64; uint32_t u32; Register():u64(0){} };
struct PPCContext { Register r1,r3,r4,r5,r6,r7,r8,r9,r10; };
#define PPC_FUNC(name) void name(PPCContext& ctx,uint8_t* base)
void KTrace(const char*){}
uint32_t GRead32(uint8_t* base,uint32_t p){uint32_t v;std::memcpy(&v,base+p,4);return __builtin_bswap32(v);}
void GWrite32(uint8_t* base,uint32_t p,uint32_t v){v=__builtin_bswap32(v);std::memcpy(base+p,&v,4);}
bool XamGuestRange(uint32_t,uint64_t,uint32_t=4);
constexpr uint32_t XAM_SUCCESS=0,XAM_INVALID_PARAMETER=87,XAM_NOT_SUPPORTED=50,XAM_INVALID_HANDLE=6,XAM_IO_PENDING=997;
HANDLE completed;
uint8_t* memory;
uint32_t eventResult=0,eventChoice=0;
bool KernelIsEventHandle(uint32_t h){return h==0xF0000100;}
bool KernelSignalEventHandle(uint32_t h){if(!KernelIsEventHandle(h))return false;eventResult=GRead32(memory,0x200);eventChoice=GRead32(memory,0x300);SetEvent(completed);return true;}
std::mutex notifyMutex;
std::vector<uint32_t> notifications;
void XamNotifyEnqueueEvent(uint32_t id,uint32_t value){std::lock_guard l(notifyMutex);if(id!=9)std::abort();notifications.push_back(value);}
@SYSUI@
std::mutex boxMutex;
std::vector<SysUiMessageBox> boxes;
bool SysUiShowMessageBox(SysUiMessageBox box){std::lock_guard l(boxMutex);if(!boxes.empty())return false;boxes.push_back(std::move(box));return true;}
@HELPERS@
'''

TEST = r'''
unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
void Text(uint32_t at,const std::wstring& s){for(wchar_t c:s){uint16_t u=__builtin_bswap16(uint16_t(c));std::memcpy(memory+at,&u,2);at+=2;}memory[at]=memory[at+1]=0;}
const wchar_t* Label(unsigned i,unsigned count,bool cancel){return cancel&&i==count-1?L"Cancel":i==0?L"Create Save Data":i==1?L"Select Storage Device":i==2?L"Third Choice":L"Fourth Choice";}
PPCContext Prepare(unsigned count=2,unsigned active=0,bool cancel=false){
    std::memset(memory,0xA5,0x1F000);ResetEvent(completed);
    Text(0x400,L"Confirm");Text(0x800,L"There is no TEKKEN TAG TOURNAMENT 2 save data.\nSave data must be created Ω.");
    for(unsigned i=0;i<count&&i<4;++i){GWrite32(memory,0x380+i*4,0x1000+i*0x400);Text(0x1000+i*0x400,Label(i,count,cancel));}
    for(unsigned i=0;i<28;i+=4)GWrite32(memory,0x200+i,0);
    GWrite32(memory,0x208,0xDEADC0DE);GWrite32(memory,0x20C,0xF0000100);GWrite32(memory,0x214,0xDEADBEEF);
    GWrite32(memory,0x300,0xABCD0123);GWrite32(memory,0x154,0x200);
    PPCContext c;c.r1.u32=0x100;c.r3.u32=0;c.r4.u32=0x400;c.r5.u32=0x800;c.r6.u32=count;c.r7.u32=0x380;c.r8.u32=active;c.r9.u32=0;c.r10.u32=0x300;return c;
}
bool Take(SysUiMessageBox& out){std::lock_guard l(boxMutex);if(boxes.size()!=1)return false;out=std::move(boxes[0]);boxes.clear();return true;}
bool NoBox(){std::lock_guard l(boxMutex);return boxes.empty();}
std::string Narrow(const wchar_t* w){std::string s;for(;*w;++w)s.push_back(char(*w));return s;}
int main(){
    memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x20000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(memory);
    DWORD old;CHECK(VirtualProtect(memory+0x1F000,4096,PAGE_NOACCESS,&old));
    completed=CreateEventW(nullptr,FALSE,FALSE,nullptr);CHECK(completed);
    size_t accepted=0;
    // Every offered button can be the answer; nothing completes until the box is answered.
    for(unsigned count=1;count<=4;++count)for(unsigned chosen=0;chosen<count;++chosen){
        auto c=Prepare(count,count-1);auto before=c;__imp__XamShowMessageBoxUI(c,memory);
        CHECK(c.r3.u32==997);CHECK(c.r4.u64==before.r4.u64&&c.r10.u64==before.r10.u64);
        CHECK(GRead32(memory,0x200)==997&&GRead32(memory,0x300)==0xABCD0123);
        CHECK(WaitForSingleObject(completed,20)==WAIT_TIMEOUT);
        // A second box while one is showing is refused and touches nothing.
        auto snapshot=std::vector<uint8_t>(memory,memory+0x1F000);
        auto second=before;__imp__XamShowMessageBoxUI(second,memory);CHECK(second.r3.u32==5);
        CHECK(!std::memcmp(snapshot.data(),memory,snapshot.size()));
        SysUiMessageBox box;CHECK(Take(box));++accepted;
        CHECK(box.text=="There is no TEKKEN TAG TOURNAMENT 2 save data.\nSave data must be created \xCE\xA9.");
        CHECK(box.buttons.size()==count&&box.active==int(count-1)&&box.cancel==-1);
        for(unsigned i=0;i<count;++i)CHECK(box.buttons[i]==Narrow(Label(i,count,false)));
        // The request owns copies: caller-side changes after the call cannot alter it.
        Text(0x800,L"Mutated");Text(0x1000,L"Mutated");
        CHECK(box.buttons[0]=="Create Save Data");
        CHECK(box.done);box.done(int(chosen));
        CHECK(WaitForSingleObject(completed,2000)==WAIT_OBJECT_0);
        CHECK(eventResult==0&&eventChoice==chosen);CHECK(GRead32(memory,0x300)==chosen);
        CHECK(GRead32(memory,0x204)==0&&GRead32(memory,0x218)==0);
        CHECK(GRead32(memory,0x208)==0xDEADC0DE&&GRead32(memory,0x214)==0xDEADBEEF);
        CHECK(GRead32(memory,0x2FC)==0xA5A5A5A5&&GRead32(memory,0x304)==0xA5A5A5A5);
        Sleep(150);                                    // the UI-closed notification follows
    }
    // A button labelled Cancel is the box's cancel answer.
    {
        auto c=Prepare(2,0,true);__imp__XamShowMessageBoxUI(c,memory);CHECK(c.r3.u32==997);
        SysUiMessageBox box;CHECK(Take(box));++accepted;CHECK(box.cancel==1&&box.buttons[1]=="Cancel");
        box.done(box.cancel);CHECK(WaitForSingleObject(completed,2000)==WAIT_OBJECT_0);CHECK(eventResult==0&&eventChoice==1);
        Sleep(150);
    }
    // All rejected inputs leave result/overlapped/event untouched and show nothing.
    for(unsigned bad=0;bad<20;++bad){
        auto c=Prepare();uint32_t expected=87;
        switch(bad){
        case 0:c.r6.u32=0;break;case 1:c.r6.u32=5;break;case 2:c.r8.u32=2;break;
        case 3:c.r7.u32=0;break;case 4:c.r7.u32=0xFFFFFFFC;break;
        case 5:c.r10.u32=0;break;case 6:c.r10.u32=0x1F000;break;
        case 7:c.r10.u32=0x204;break;case 8:GWrite32(memory,0x154,0x1F000);break;
        case 9:c.r4.u32=0x1EFFF;break;case 10:c.r5.u32=0x1F000;break;
        case 11:GWrite32(memory,0x380,0);break;case 12:Text(0x1000,L"");break;
        case 13:Text(0x1000,std::wstring(256,L'x'));break;
        case 14:Text(0x400,std::wstring(256,L'x'));break;
        case 15:GWrite32(memory,0x210,0x82250000);expected=50;break;
        case 16:GWrite32(memory,0x20C,0xBAD);expected=6;break;
        case 17:GWrite32(memory,0x154,0);expected=50;break;
        case 18:c.r1.u32=0xFFFFFFF0;break;case 19:c.r7.u32=0x1F000;break;
        }
        auto original=std::vector<uint8_t>(memory,memory+0x1F000);
        __imp__XamShowMessageBoxUI(c,memory);CHECK(c.r3.u32==expected);
        CHECK(!std::memcmp(original.data(),memory,original.size()));CHECK(WaitForSingleObject(completed,0)==WAIT_TIMEOUT);CHECK(NoBox());
    }
    {std::lock_guard l(notifyMutex);CHECK(notifications.size()==accepted*2);for(size_t i=0;i<notifications.size();++i)CHECK(notifications[i]==(i%2?0:1));}
    CloseHandle(completed);VirtualFree(memory,0,MEM_RELEASE);
    std::printf("PASS %u message-box checks: UTF-16 -> UTF-8 copies, 1-4 choices, default focus, cancel, async/event order, bounds, busy, lifetime\n",checks);
}
'''


def main():
    source = (RUNTIME / "kernel/kernel_ui.cpp").read_text(encoding="utf-8-sig")
    production = re.sub(r'^#include "[^\n]+"\n', '', source, flags=re.M)
    sysui = (RUNTIME / "kernel/sys_ui.h").read_text(encoding="utf-8-sig").replace("#pragma once", "")
    content = (RUNTIME / "kernel/kernel_content.cpp").read_text(encoding="utf-8-sig")
    helpers = content[content.index("bool XamGuestRange("):content.index("bool TryCloseContentEnumerator(")]
    prefix = PREFIX.replace("@SYSUI@", sysui).replace("@HELPERS@", helpers)
    with tempfile.TemporaryDirectory(prefix="ttt2-message-box-") as temp:
        folder = Path(temp)
        executable = compile_one(folder, "message_box", prefix + production + TEST, [])
        result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        print(result.stdout, end="")
        old = production[:production.index("PPC_FUNC(__imp__XamShowMessageBoxUI)")] + "PPC_FUNC(__imp__XamShowMessageBoxUI){ctx.r3.u32=0;}\n"
        executable = compile_one(folder, "message_box_old", prefix + old + TEST, [])
        result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=5)
        if result.returncode == 0 or "c.r3.u32==997" not in result.stderr:
            raise RuntimeError("Original success stub negative control failed: " + result.stdout + result.stderr)
        print("PASS negative control: success-without-box stub rejected")


if __name__ == "__main__":
    main()
