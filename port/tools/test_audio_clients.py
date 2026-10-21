"""Exercise the production audio registration/pump lifecycle without game data.

The only replacement in kernel_audio.cpp is host pump-thread creation: the test
calls the production per-frame pump deterministically. Guest ABI, client registry,
callback dispatch, submit and unregister execute unchanged. A singleton negative
control must fail. Per-client ABI/handle reference: Xenia xboxkrnl_audio.cc and
AudioSystem::RegisterClient in src/xenia/apu/audio_system.cc (8 clients).
"""
from pathlib import Path
import re
import subprocess
import tempfile
from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
union Reg { uint32_t u32; uint64_t u64; };
struct PPCContext { Reg r3{}, r4{}; };
#define PPC_FUNC(n) void n(PPCContext& ctx,uint8_t* base)
static std::array<uint8_t,1<<20> memory;
uint8_t* g_base=memory.data();
constexpr uint32_t STATUS_SUCCESS=0;
void KTrace(const char*) {}
uint32_t GRead32(uint8_t* b,uint32_t a) { uint32_t v; std::memcpy(&v,b+a,4);return __builtin_bswap32(v); }
void GWrite32(uint8_t* b,uint32_t a,uint32_t v) { v=__builtin_bswap32(v);std::memcpy(b+a,&v,4); }
uint32_t GuestAddr(uint8_t* b,void* p) { return uint32_t((uint8_t*)p-b); }
std::atomic<unsigned> checks{0};
#define CHECK(x) do { ++checks; if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::abort();} } while(0)
struct Heap {
 std::mutex mutex; uint32_t next=0x10000; bool fail=false; std::map<void*,bool> blocks;
 void* Alloc(size_t n,size_t align) {std::lock_guard l(mutex);if(fail)return nullptr;next=(next+uint32_t(align)-1)&~uint32_t(align-1);void* p=g_base+next;next+=uint32_t(n);CHECK(next<memory.size());blocks[p]=true;return p;}
 void Free(void* p) {std::lock_guard l(mutex);CHECK(blocks.count(p)&&blocks[p]);blocks[p]=false;std::memset(p,0xDD,8);}
 bool live(uint32_t cell) {std::lock_guard l(mutex);return blocks[g_base+cell];}
 size_t liveCount() {std::lock_guard l(mutex);size_t n=0;for(auto&[p,on]:blocks)n+=on;return n;}
} g_virtualHeap;
unsigned testPumpStarts=0;
std::function<void(uint32_t,uint32_t)> callback;
void GuestCall(uint32_t fn,uint32_t cell) {CHECK(g_virtualHeap.live(cell));callback(fn,cell);}
// Host output is tested separately by test_audio_output.py. Keep this fixture
// focused on the unchanged guest registry/callback lifecycle without a device.
constexpr size_t AUDIO_PCM_BYTES=6144;
struct AudioOutputStats { unsigned ready=0,failed=0,queued=0,deviceQueued=0,renderCredits=0;uint64_t received=0,submitted=0,completed=0,dropped=0,nonzero=0,nonfinite=0,starvationPasses=0,starvationBytes=0; };
struct AudioOutputClient { bool closed=false,credit=true;explicit AudioOutputClient(uint32_t){} void Submit(const uint8_t*){} void Close(){closed=true;} bool TryAcquireRenderCredit(uint64_t){return credit;} AudioOutputStats Stats()const{return{};} };
uint64_t AudioOutputWakeVersion(){return 0;}void WaitAudioOutputWake(uint64_t,uint64_t){}
@PRODUCTION@
uint32_t reg(uint32_t fn,uint32_t param,uint32_t* status=nullptr) {
 GWrite32(g_base,0x100,fn);GWrite32(g_base,0x104,param);GWrite32(g_base,0x108,0xBADF00D);
 PPCContext c;c.r3.u32=0x100;c.r4.u32=0x108;__imp__XAudioRegisterRenderDriverClient(c,g_base);
 if(status)*status=c.r3.u32;else CHECK(c.r3.u32==0);return GRead32(g_base,0x108);
}
uint32_t unreg(uint32_t h) {PPCContext c;c.r3.u32=h;__imp__XAudioUnregisterRenderDriverClient(c,g_base);return c.r3.u32;}
uint32_t submit(uint32_t h) {PPCContext c;c.r3.u32=h;c.r4.u32=0x400;__imp__XAudioSubmitRenderDriverFrame(c,g_base);return c.r3.u32;}
void noClients() {CHECK(g_virtualHeap.liveCount()==0);for(auto& c:g_xaClients)CHECK(!c);}
int main() {
 // Two engines retain distinct stable wrapped contexts across many heartbeats.
 uint32_t a=reg(0x1000,0xAAAA), b=reg(0x2000,0xBBBB);
 CHECK(a==0x41550000&&b==0x41550001&&testPumpStarts==1);
 uint32_t ca=FindAudioClient(a)->paramCell,cb=FindAudioClient(b)->paramCell;
 CHECK(ca!=cb);CHECK(GRead32(g_base,ca)==0xAAAA&&GRead32(g_base,cb)==0xBBBB);
 std::array<unsigned,2> called{};
 callback=[&](uint32_t fn,uint32_t cell) {unsigned i=fn==0x1000?0:1;CHECK(fn==(i?0x2000u:0x1000u));CHECK(cell==(i?cb:ca));CHECK(GRead32(g_base,cell)==(i?0xBBBBu:0xAAAAu));++called[i];CHECK(submit(i?b:a)==0);};
 for(int n=0;n<32;++n)PumpAudioClients();
 CHECK(called[0]==32&&called[1]==32);CHECK(FindAudioClient(a)->submitted==32&&FindAudioClient(b)->submitted==32);
 FindAudioClient(b)->output->credit=false;CHECK(PumpAudioClients());CHECK(called[0]==33&&called[1]==32);
 FindAudioClient(a)->output->credit=false;CHECK(!PumpAudioClients());CHECK(called[0]==33&&called[1]==32);
 FindAudioClient(a)->output->credit=true;
 CHECK(unreg(b)==0);CHECK(!g_virtualHeap.live(cb));PumpAudioClients();CHECK(called[0]==34&&called[1]==32);
 CHECK(submit(b)==AUDIO_INVALID_HANDLE);CHECK(unreg(b)==AUDIO_INVALID_HANDLE);
 CHECK(unreg(a)==0);noClients();
 // Unsupported handles and allocation/capacity failures leave output untouched.
 for(uint32_t h:{0u,0x44415544u,0x41550008u,0x4155FFFFu}) {CHECK(submit(h)==AUDIO_INVALID_HANDLE);CHECK(unreg(h)==AUDIO_INVALID_HANDLE);}
 uint32_t status=0;g_virtualHeap.fail=true;CHECK(reg(0x1000,1,&status)==0xBADF00D&&status==AUDIO_NO_MEMORY);g_virtualHeap.fail=false;
 CHECK(reg(0,1,&status)==0xBADF00D&&status==AUDIO_INVALID_PARAMETER);
 CHECK(reg(0x1001,1,&status)==0xBADF00D&&status==AUDIO_INVALID_PARAMETER);
 for(unsigned bad=0;bad<4;++bad) {PPCContext c;c.r3.u32=bad==0?0:bad==1?0x101:0x100;c.r4.u32=bad==2?0:bad==3?0x109:0x108;__imp__XAudioRegisterRenderDriverClient(c,g_base);CHECK(c.r3.u32==AUDIO_INVALID_PARAMETER);}
 std::vector<uint32_t> handles;for(unsigned i=0;i<8;++i)handles.push_back(reg(0x1000+i*4,100+i));
 CHECK(reg(0x2000,9,&status)==0xBADF00D&&status==AUDIO_NO_MEMORY);
 unsigned all=0;callback=[&](uint32_t fn,uint32_t cell){CHECK(GRead32(g_base,cell)==100+(fn-0x1000)/4);++all;};PumpAudioClients();CHECK(all==8);
 for(auto h:handles)CHECK(unreg(h)==0);noClients();
 // Unregister waits for an in-flight callback. That callback can submit while
 // unregister is pending, and its context remains alive until callback exit.
 a=reg(0x1000,77);ca=FindAudioClient(a)->paramCell;
 std::mutex m;std::condition_variable cv;bool entered=false,doSubmit=false,submitted=false,release=false;
 callback=[&](uint32_t,uint32_t cell){
  {std::unique_lock l(m);entered=true;cv.notify_all();CHECK(cv.wait_for(l,std::chrono::seconds(3),[&]{return doSubmit;}));}
  CHECK(submit(a)==0);CHECK(g_virtualHeap.live(cell));
  {std::unique_lock l(m);submitted=true;cv.notify_all();CHECK(cv.wait_for(l,std::chrono::seconds(3),[&]{return release;}));}
 };
 std::thread pumping([]{PumpAudioClients();});
 {std::unique_lock l(m);CHECK(cv.wait_for(l,std::chrono::seconds(3),[&]{return entered;}));}
 std::promise<void> unregisterStarted;auto started=unregisterStarted.get_future();
 auto retiring=std::async(std::launch::async,[&]{unregisterStarted.set_value();return unreg(a);});
 started.wait();CHECK(retiring.wait_for(std::chrono::milliseconds(25))==std::future_status::timeout);CHECK(g_virtualHeap.live(ca));
 {std::lock_guard l(m);doSubmit=true;}cv.notify_all();
 {std::unique_lock l(m);CHECK(cv.wait_for(l,std::chrono::seconds(3),[&]{return submitted;}));release=true;}cv.notify_all();
 pumping.join();CHECK(retiring.get()==0);CHECK(!g_virtualHeap.live(ca));PumpAudioClients();noClients();
 // A callback can retire itself without deadlock; its wrapped context remains
 // live until that callback and the pump snapshot have finished.
 a=reg(0x1000,88);ca=FindAudioClient(a)->paramCell;unsigned selfCalls=0;
 callback=[&](uint32_t,uint32_t cell){++selfCalls;auto retained=FindAudioClient(a);CHECK(!retained->output->closed);CHECK(unreg(a)==0);CHECK(retained->output->closed);CHECK(g_virtualHeap.live(cell));CHECK(GRead32(g_base,cell)==88);};
 PumpAudioClients();CHECK(selfCalls==1&&!g_virtualHeap.live(ca));PumpAudioClients();CHECK(selfCalls==1);noClients();
 // An old snapshot must not call a removed client, even when its slot is
 // reused during an earlier callback in that same frame.
 a=reg(0x1000,11);b=reg(0x2000,22);cb=FindAudioClient(b)->paramCell;uint32_t c=0;unsigned newCalls=0;
 callback=[&](uint32_t fn,uint32_t cell){if(fn==0x1000){if(!c){CHECK(unreg(b)==0);CHECK(g_virtualHeap.live(cb));c=reg(0x3000,33);CHECK(c==b);}}else {CHECK(fn==0x3000&&GRead32(g_base,cell)==33);++newCalls;}};
 PumpAudioClients();CHECK(newCalls==0&&!g_virtualHeap.live(cb));PumpAudioClients();CHECK(newCalls==1);
 CHECK(unreg(a)==0&&unreg(c)==0);noClients();CHECK(testPumpStarts==1);
 std::printf("PASS %u audio-client checks (ABI, 8 clients, lifecycle, concurrent unregister, self-unregister, stale snapshot)\n",checks.load());
}
'''

def main():
    source = (ROOT / "port/src/runtime/kernel/kernel_audio.cpp").read_text()
    source = re.sub(r'^#include "[^\n]+"\n', '', source, flags=re.M)
    start = 'std::thread(AudioPumpThread).detach();'
    assert source.count(start) == 1
    source = source.replace(start, '++testPumpStarts; // host startup replaced; production pump invoked directly')
    with tempfile.TemporaryDirectory(prefix="ttt2_audio_clients_") as tmp:
        folder=Path(tmp)
        executable=compile_one(folder,"audio_clients",HARNESS.replace("@PRODUCTION@",source),[])
        subprocess.run([str(executable)],check=True,timeout=20)
        # Recreate the original singleton replacement on every registration.
        assert source.count('g_xaClients[index] = client;') == 1
        singleton=source.replace('g_xaClients[index] = client;', 'g_xaClients.fill({}); g_xaClients[0] = client;')
        executable=compile_one(folder,"audio_singleton",HARNESS.replace("@PRODUCTION@",singleton),[])
        result=subprocess.run([str(executable)],capture_output=True,text=True,timeout=20)
        if result.returncode==0:raise SystemExit("Singleton negative control unexpectedly passed")
        print("PASS negative control: replacing the first audio client with the second is detected")

if __name__ == "__main__": main()
