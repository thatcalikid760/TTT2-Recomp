"""Production local-content ABI and completion tests without game data or saves.

Compiles actual kernel objects, content implementation and selector import. Tests
closable empty enumeration, output bounds, error completion, real event signaling,
concurrent handles and the guest's bounded equivalent of its enumeration loop.
Old selector-return and success-only enumeration negative controls must fail.
"""
from pathlib import Path
import re
import subprocess
import tempfile

from test_kernel_waits import SHARED, RECOMP, ROOT
from test_vcmpgtuh import compile_one

CHECKER = r'''
#include "@OBJECTS@"
@CONTENT@
#include "@STORE@"
#include <array>
#include <future>
#include <vector>
bool WTrace() { return false; }
bool TryCloseGuestFile(uint32_t) { return false; }
// Live enumerators (xlive.cpp) are not part of this test; the signed-in profile is the old save's.
bool XLiveEnumerate(uint8_t*,uint32_t,uint32_t,uint32_t,uint32_t&,uint32_t&) { return false; }
bool XLiveCloseHandle(uint32_t,uint32_t&) { return false; }
namespace profiles { uint64_t ActiveXuid() { return 0xB13EBABEBABEBABEull; } }
uint32_t FlushSaveFiles(const std::filesystem::path&,bool) { return 0; }
static void Reporter(PPCContext&, uint8_t*) { std::abort(); }
PPCFunc* const g_unmappedReporter = Reporter;
std::vector<std::pair<uint32_t,uint32_t>> notifications;
void XamNotifyEnqueueEvent(uint32_t id,uint32_t value) { notifications.emplace_back(id,value); }
@SELECTOR@
static unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x); std::abort();} }while(0)
alignas(16) std::array<uint8_t,4096> memory;
uint8_t* base=memory.data();
void Reset() {memory.fill(0xA5);notifications.clear();}
void Word(uint32_t p,uint32_t expected) {
 CHECK(GRead32(base,p)==expected);
 CHECK(GRead32(base,p-4)==0xA5A5A5A5 && GRead32(base,p+4)==0xA5A5A5A5);
}
void Overlapped(uint32_t event=0,uint32_t routine=0) {
 for(unsigned p=0x200;p<0x21C;p+=4)GWrite32(base,p,0);
 GWrite32(base,0x200,997);GWrite32(base,0x208,0x12345678);
 GWrite32(base,0x20C,event);GWrite32(base,0x210,routine);
 GWrite32(base,0x214,0x98765432);GWrite32(base,0x218,0xBADF00D);
}
void Completed(uint32_t result,uint32_t extended,uint32_t event=0) {
 CHECK(GRead32(base,0x200)==result && GRead32(base,0x204)==0);
 CHECK(GRead32(base,0x218)==extended);
 CHECK(GRead32(base,0x208)==0x12345678 && GRead32(base,0x20C)==event);
 CHECK(GRead32(base,0x210)==0 && GRead32(base,0x214)==0x98765432);
 CHECK(GRead32(base,0x1FC)==0xA5A5A5A5 && GRead32(base,0x21C)==0xA5A5A5A5);
}
PPCContext EnumContext(uint32_t handle,uint32_t countOut=0x180,uint32_t ov=0) {
 PPCContext c{};c.r3.u32=handle;c.r4.u32=0;c.r5.u32=0x400;c.r6.u32=308;
 c.r7.u32=countOut;c.r8.u32=ov;return c;
}
uint32_t New(uint32_t count=1,uint32_t user=0,uint32_t device=1,uint32_t type=1) {
 PPCContext c{};c.r3.u32=user;c.r4.u32=device;c.r5.u32=type;c.r6.u32=0;
 c.r7.u32=count;c.r8.u32=0x100;c.r9.u32=0x120;
 __imp__XamContentCreateEnumerator(c,base);CHECK(c.r3.u32==0);
 Word(0x100,308*count);auto h=GRead32(base,0x120);Word(0x120,h);
 CHECK(h>=0xD1000000 && h<0xD2000000);return h;
}
uint32_t Close(uint32_t h) { PPCContext c{};c.r3.u32=h;__imp__NtClose(c,base);return c.r3.u32; }
int main() {
 Reset();PPCContext c{};c.r3.u32=0;c.r4.u32=1;c.r7.u32=0x100;
 __imp__XamShowDeviceSelectorUI(c,base);CHECK(c.r3.u32==0);Word(0x100,1);
 CHECK(notifications.size()==2 && notifications[0].first==9 && notifications[0].second==1 && notifications[1].second==0);
 auto* event=new KEvent(false,false);uint32_t eh=KernelObjectHandle(event);
 Reset();Overlapped(eh);c={};c.r7.u32=0x100;c.r8.u32=0x200;
 __imp__XamShowDeviceSelectorUI(c,base);CHECK(c.r3.u32==997);Word(0x100,1);Completed(0,0,eh);
 CHECK(event->Wait(0)==0 && event->Wait(0)==STATUS_TIMEOUT);
 // The real guest rejects a synchronous return when its record was supplied.
 CHECK(c.r3.u32==997 && GRead32(base,0x200)!=997);
 for(uint32_t ptr:{0u,0x101u,0xFFFFFFFDu}) {
  Reset();c={};c.r7.u32=ptr;auto before=memory;
  __imp__XamShowDeviceSelectorUI(c,base);CHECK(c.r3.u32==87 && memory==before && notifications.empty());
 }
 Reset();Overlapped(0,0x82250100);c={};c.r7.u32=0x100;c.r8.u32=0x200;
 auto before=memory;__imp__XamShowDeviceSelectorUI(c,base);CHECK(c.r3.u32==50 && memory==before && notifications.empty());
 auto* mutant=new KMutant();uint32_t mh=KernelObjectHandle(mutant);
 CHECK(!KernelIsEventHandle(mh) && KernelIsEventHandle(eh));
 KernelSignalEventHandle(mh);CHECK(!mutant->held); // no wrong-type cast or side effect
 Reset();Overlapped(mh);c={};c.r7.u32=0x100;c.r8.u32=0x200;before=memory;
 __imp__XamShowDeviceSelectorUI(c,base);CHECK(c.r3.u32==6 && memory==before);
 Reset();c={};c.r3.u32=1;c.r7.u32=0x100;before=memory;
 __imp__XamShowDeviceSelectorUI(c,base);CHECK(c.r3.u32==1317 && memory==before);
 Reset();c={};c.r3.u32=1;__imp__XamContentGetDeviceState(c,base);CHECK(c.r3.u32==0);
 Reset();Overlapped(eh);c={};c.r3.u32=1;c.r4.u32=0x200;
 __imp__XamContentGetDeviceState(c,base);CHECK(c.r3.u32==997);Completed(0,0,eh);CHECK(event->Wait(0)==0);
 for(uint32_t id:{0u,2u,0xFFFFFFFFu}) {
  c={};c.r3.u32=id;__imp__XamContentGetDeviceState(c,base);CHECK(c.r3.u32==1167);
  Reset();Overlapped();c={};c.r3.u32=id;c.r4.u32=0x200;
  __imp__XamContentGetDeviceState(c,base);CHECK(c.r3.u32==997);Completed(1627,1167);
 }

 Reset();const auto first=New();const auto second=New(2);CHECK(first!=second);
 c=EnumContext(first);before=memory;__imp__XamEnumerate(c,base);
 CHECK(c.r3.u32==18);Word(0x180,0);CHECK(std::memcmp(base+0x400,before.data()+0x400,616)==0);
 unsigned iterations=0;
 do {c=EnumContext(first);__imp__XamEnumerate(c,base);++iterations;}while(!c.r3.u32 && iterations<10);
 CHECK(iterations==1 && c.r3.u32==18);
 c=EnumContext(second);__imp__XamEnumerate(c,base);CHECK(c.r3.u32==122);Word(0x180,0);
 c=EnumContext(second);c.r6.u32=616;__imp__XamEnumerate(c,base);CHECK(c.r3.u32==18);
 CHECK(Close(first)==0);CHECK(Close(first)==0xC0000008u);
 c=EnumContext(first);__imp__XamEnumerate(c,base);CHECK(c.r3.u32==6);Word(0x180,0);
 c=EnumContext(second);c.r6.u32=616;__imp__XamEnumerate(c,base);CHECK(c.r3.u32==18);
 CHECK(Close(second)==0);

 Reset();const auto third=New();Overlapped(eh);c=EnumContext(third,0,0x200);
 __imp__XamEnumerate(c,base);CHECK(c.r3.u32==997);Completed(18,0x80070012u,eh);
 CHECK(event->Wait(0)==0);Word(0x180,0xA5A5A5A5);
 c=EnumContext(third,0);__imp__XamEnumerate(c,base);CHECK(c.r3.u32==18);
 for(uint32_t ptr:{0u,0xFFFFFF00u}) {
  c=EnumContext(third);c.r5.u32=ptr;__imp__XamEnumerate(c,base);CHECK(c.r3.u32==87);Word(0x180,0);
 }
 c=EnumContext(third);c.r4.u32=1;before=memory;__imp__XamEnumerate(c,base);CHECK(c.r3.u32==87 && memory==before);
 c=EnumContext(third,0x180,0x200);before=memory;__imp__XamEnumerate(c,base);CHECK(c.r3.u32==87 && memory==before);
 CHECK(Close(third)==0);
 for(uint32_t count:{0u,0xFFFFFFFFu,13944700u}) {
  Reset();c={};c.r3.u32=0;c.r4.u32=1;c.r5.u32=1;c.r7.u32=count;c.r8.u32=0x100;c.r9.u32=0x120;
  __imp__XamContentCreateEnumerator(c,base);CHECK(c.r3.u32==87);Word(0x100,0);Word(0x120,0);
 }
 for(uint32_t user:{1u,2u,3u,4u}) {
  Reset();c={};c.r3.u32=user;c.r4.u32=1;c.r5.u32=1;c.r7.u32=1;c.r8.u32=0x100;c.r9.u32=0x120;
  __imp__XamContentCreateEnumerator(c,base);CHECK(c.r3.u32==1317);Word(0x100,0);Word(0x120,0);
 }
 for(uint32_t user:{0u,0xFEu,0xFFu}) {Reset();CHECK(Close(New(1,user,0,2))==0);}
 Reset();c={};c.r9.u32=0;before=memory;__imp__XamContentCreateEnumerator(c,base);CHECK(c.r3.u32==87 && memory==before);

 // No save writes or false dispositions while persistence is absent.
 for(uint32_t mode=0;mode<=6;++mode) {
  Reset();c={};c.r1.u32=0x800;GWrite32(base,0x854,0);
  c.r4.u32=0x300;c.r5.u32=0x400;c.r6.u32=mode|16;c.r7.u32=0x100;c.r8.u32=0x120;
  std::memcpy(base+0x300,"tag25save",10);
  __imp__XamContentCreateEx(c,base);
  CHECK(c.r3.u32==(mode==0||mode==6?87:mode==3||mode==5?3:5));Word(0x100,0);Word(0x120,0);
 }
 Reset();Overlapped(eh);c={};c.r1.u32=0x800;GWrite32(base,0x854,0x200);
 c.r4.u32=0x300;c.r5.u32=0x400;c.r6.u32=2;c.r7.u32=0x100;c.r8.u32=0x120;
 std::memcpy(base+0x300,"tag25save",10);
 __imp__XamContentCreateEx(c,base);CHECK(c.r3.u32==997);Completed(5,0x80070005u,eh);CHECK(event->Wait(0)==0);
 CHECK(g_contentEnumerators.empty());
 // Registry operations use real synchronization and isolated guest outputs.
 std::vector<std::future<bool>> work;
 for(unsigned t=0;t<8;++t)work.emplace_back(std::async(std::launch::async,[]{
  alignas(16) std::array<uint8_t,1024> mem{};auto* b=mem.data();
  for(unsigned i=0;i<100;++i) {
   PPCContext q{};q.r4.u32=1;q.r5.u32=1;q.r7.u32=1;q.r8.u32=64;q.r9.u32=80;
   __imp__XamContentCreateEnumerator(q,b);if(q.r3.u32)return false;
   const auto h=GRead32(b,80);q={};q.r3.u32=h;q.r5.u32=128;q.r6.u32=308;q.r7.u32=64;
   __imp__XamEnumerate(q,b);if(q.r3.u32!=18 || GRead32(b,64))return false;
   q.r3.u32=h;__imp__NtClose(q,b);if(q.r3.u32)return false;
  } return true;
 }));
 for(auto& w:work)CHECK(w.get());CHECK(g_contentEnumerators.empty());
 std::printf("PASS %u content checks + 800 concurrent create/enumerate/close cycles\n",checks);
}
'''


def main():
    source = (ROOT / "port/src/runtime/kernel/kernel_xam.cpp").read_text()
    selector = "\n".join(re.search(r"PPC_FUNC\(__imp__" + name + r"\)\n\{.*?\n\}", source, re.S).group()
                         for name in ("XamShowDeviceSelectorUI", "XamContentGetDeviceState"))
    content = (ROOT / "port/src/runtime/kernel/kernel_content.cpp").read_text()
    with tempfile.TemporaryDirectory(prefix="ttt2-content-") as temp:
        folder = Path(temp)
        (folder / "ppc_recomp_shared.h").write_text(SHARED)
        includes = [folder, RECOMP / "XenonUtils", RECOMP / "thirdparty/simde",
                    ROOT / "port/src/runtime", ROOT / "port/src/runtime/kernel"]
        production = CHECKER.replace("@OBJECTS@", (ROOT / "port/src/runtime/kernel/kernel_objects.cpp").as_posix())
        production = production.replace("@CONTENT@", content).replace("@SELECTOR@", selector)
        production = production.replace("@STORE@", (ROOT / "port/src/runtime/kernel/save_store.cpp").as_posix())
        exe = compile_one(folder, "content", production, includes)
        subprocess.run([str(exe)], check=True, timeout=20)
        for name, old in [
            ("old_selector", production.replace("ctx.r3.u32 = XamCompleteOverlapped(base, ovl, 0, 0, 0);",
                                                "XamCompleteOverlapped(base, ovl, 0, 0, 0); ctx.r3.u32 = 0;")),
            ("old_enum", production.replace("XAM_NO_MORE_FILES = 18", "XAM_NO_MORE_FILES = 0")),
        ]:
            assert old != production
            exe = compile_one(folder, name, old, includes)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
            if result.returncode == 0:
                raise RuntimeError(name + " negative control unexpectedly passed")
            print(name + " negative control rejected: " + result.stderr.strip().splitlines()[-1])


if __name__ == "__main__":
    main()
