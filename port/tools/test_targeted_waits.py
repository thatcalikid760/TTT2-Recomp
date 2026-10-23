"""Production targeted multiwait notification and registration lifetime tests.

Exercises unrelated real transitions, signal/registration races, overlapping
subscriptions, timeout unlinking, retained objects across handle close, and all
64 wait slots. No game execution, memory patching, or full port build.
"""
from pathlib import Path
import subprocess
import tempfile

from test_kernel_waits import CHECKER as WAIT_CHECKER, SHARED, ROOT, RECOMP
from test_vcmpgtuh import compile_one


CHECKER = WAIT_CHECKER.split('int main() {',1)[0] + r'''
static unsigned Subscribers(KObject& object) {
    std::lock_guard lock(g_dispatchMutex);
    unsigned count=0;for(auto* link=object.multiWaiters;link;link=link->next)++count;
    return count;
}
int main() {
    ObservedEvent watched(false);KObject* list[]={&watched};
    auto waiter=std::async(std::launch::async,[&]{return WaitMultiple(list,1,true,5000);});
    AwaitScans(watched,1);CHECK(Subscribers(watched)==1);
    const unsigned scans=watched.scans;
    uint64_t generation,notifications;
    {std::lock_guard lock(g_dispatchMutex);generation=g_dispatchGeneration;notifications=g_dispatchNotifications;}
    KEvent unrelated(false,false);KSemaphore semaphore(0,1);KMutant mutant;
    for(unsigned i=0;i<10000;++i) {
        unrelated.Set();unrelated.Reset();
        semaphore.Release(1);CHECK(semaphore.Wait(0)==0);
        CHECK(mutant.Wait(0)==0);mutant.Release();
    }
    // Let a wrongly broadcast waiter run before checking its observation count.
    std::this_thread::sleep_for(20ms);
    {std::lock_guard lock(g_dispatchMutex);
     CHECK(g_dispatchGeneration-generation==30000);
     CHECK(g_dispatchNotifications==notifications);}
    CHECK(watched.scans==scans);
    CHECK(waiter.wait_for(0ms)==std::future_status::timeout);
    watched.Set();CHECK(waiter.get()==0);CHECK(Subscribers(watched)==0);
    {std::lock_guard lock(g_dispatchMutex);CHECK(g_dispatchNotifications-notifications==1);}

    // A new signal can race registration, its first scan, or entry into sleep.
    // Every route must either observe retained state or receive its own wake.
    for(unsigned i=0;i<256;++i) {
        ObservedEvent event(false);KObject* pair[]={&event};
        auto wait=std::async(std::launch::async,[&]{return WaitMultiple(pair,1,true,1000);});
        if(i&1)AwaitScans(event,1);
        event.Set();CHECK(wait.get()==0);CHECK(Subscribers(event)==0);
        uint64_t before;
        {std::lock_guard lock(g_dispatchMutex);before=g_dispatchNotifications;}
        event.Set();event.Pulse();
        {std::lock_guard lock(g_dispatchMutex);CHECK(g_dispatchNotifications==before);}
    }

    // Explicit attach order: tail, middle, head. Timeout removes middle then
    // head while another registration remains subscribed; its link stays valid.
    ObservedEvent shared(true);KObject* overlap[]={&shared};
    auto tail=std::async(std::launch::async,[&]{return WaitMultiple(overlap,1,true,2000);});
    AwaitScans(shared,1);
    auto middle=std::async(std::launch::async,[&]{return WaitMultiple(overlap,1,true,40);});
    AwaitScans(shared,2);
    auto head=std::async(std::launch::async,[&]{return WaitMultiple(overlap,1,true,100);});
    AwaitScans(shared,3);CHECK(Subscribers(shared)==3);
    CHECK(middle.get()==STATUS_TIMEOUT);CHECK(Subscribers(shared)==2);
    CHECK(head.get()==STATUS_TIMEOUT);CHECK(Subscribers(shared)==1);
    shared.Set();CHECK(tail.get()==0);CHECK(Subscribers(shared)==0);

    // Closing a handle preserves the pre-existing retained-object wait behavior.
    // The registration owns no handle and detaches before its local CV dies.
    auto* retained=new ObservedEvent(false);
    uint32_t handle=KernelObjectHandle(retained);KObject* closing[]={retained};
    auto closeWait=std::async(std::launch::async,[&]{return WaitMultiple(closing,1,true,1000);});
    AwaitScans(*retained,1);
    PPCContext close{};close.r3.u32=handle;uint8_t dummy[16]{};
    __imp__NtClose(close,dummy);CHECK(close.r3.u32==0);CHECK(KernelGetObject(handle)==nullptr);
    retained->Set();CHECK(closeWait.get()==0);CHECK(Subscribers(*retained)==0);
    delete retained;

    // Maximum-size registrations, plus successful WaitAll consumption of all
    // inputs, prove every link is removed on both finite and immediate paths.
    std::array<std::unique_ptr<KEvent>,64> events;
    std::array<KObject*,64> objects;
    for(unsigned i=0;i<64;++i){events[i]=std::make_unique<KEvent>(false,false);objects[i]=events[i].get();}
    CHECK(WaitMultiple(objects.data(),64,true,0)==STATUS_TIMEOUT);
    for(auto& event:events)CHECK(Subscribers(*event)==0);
    for(auto& event:events)event->Set();
    CHECK(WaitMultiple(objects.data(),64,false,0)==0);
    for(auto& event:events){CHECK(Subscribers(*event)==0);CHECK(event->Wait(0)==STATUS_TIMEOUT);}
    std::printf("PASS %u targeted-wait checks: 30,000 unrelated transitions/zero wakes, notify races, timeout unlink order, close lifetime, 64-object cleanup\n",checks);
}
'''


def main():
    objects=ROOT/'port/src/runtime/kernel/kernel_objects.cpp'
    production=objects.read_text(encoding='utf-8-sig')
    with tempfile.TemporaryDirectory(prefix='ttt2-targeted-waits-') as temp:
        folder=Path(temp)
        (folder/'ppc_recomp_shared.h').write_text(SHARED,encoding='utf-8')
        includes=[folder,RECOMP/'XenonUtils',RECOMP/'thirdparty/simde',ROOT/'port/src/runtime',objects.parent]
        exe=compile_one(folder,'targeted',CHECKER.replace('@OBJECTS@',objects.as_posix()),includes)
        subprocess.run([str(exe)],check=True,timeout=25)
        # Restore the old shared-CV/generation wake source, retaining unrelated
        # production code and registrations. It must cause extra predicate scans.
        old=production.replace('std::mutex g_dispatchMutex;',
            'std::mutex g_dispatchMutex;\nstd::condition_variable g_dispatchCv;')
        old=old.replace('++g_dispatchGeneration;','++g_dispatchGeneration; g_dispatchCv.notify_all();')
        old=old.replace('const uint64_t generation = registration.generation;',
            'const uint64_t generation = g_dispatchGeneration;')
        old=old.replace('generation != registration.generation','generation != g_dispatchGeneration')
        old=old.replace('registration.cv.wait','g_dispatchCv.wait')
        path=folder/'old_broadcast.cpp';path.write_text(old,encoding='utf-8')
        exe=compile_one(folder,'broadcast',CHECKER.replace('@OBJECTS@',path.as_posix()),includes)
        run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=25)
        assert run.returncode and 'watched.scans==scans' in run.stderr,run.stdout+run.stderr
        print('PASS global-broadcast negative control detected by unrelated predicate scans')


if __name__=='__main__':main()
