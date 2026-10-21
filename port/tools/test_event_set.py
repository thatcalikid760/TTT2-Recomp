"""Production event Set coalescing, concurrent wake and overlap regressions.

An already-signaled event creates no additional signal entitlement. Test the
actual dispatcher and count notifications, including WaitAll's unconsumed first
event. This does not run a game or change its process scheduling.
Reference: https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setevent
"""
from pathlib import Path
import subprocess
import tempfile

from test_kernel_waits import CHECKER as WAIT_CHECKER, SHARED, ROOT, RECOMP
from test_vcmpgtuh import compile_one


CHECKER = WAIT_CHECKER.split('int main() {',1)[0] + r'''
int main() {
    for(bool manual:{false,true}) {
        KEvent signaled(manual,false);
        uint64_t before=g_dispatchGeneration;
        signaled.Set();
        for(unsigned i=0;i<10000;++i) signaled.Set();
        CHECK(g_dispatchGeneration-before==1);
        CHECK(signaled.Wait(0)==STATUS_SUCCESS);
        CHECK(signaled.Wait(0)==(manual?STATUS_SUCCESS:STATUS_TIMEOUT));
        signaled.Reset();signaled.Set();
        CHECK(g_dispatchGeneration-before==2);

        // Four producers repeatedly Set one retained signal. It remains a
        // single entitlement, with no notifications caused by redundant Sets.
        std::vector<std::thread> producers;
        before=g_dispatchGeneration;
        for(unsigned t=0;t<4;++t)producers.emplace_back([&]{for(unsigned i=0;i<5000;++i)signaled.Set();});
        for(auto& t:producers)t.join();
        CHECK(g_dispatchGeneration==before);
        CHECK(signaled.Wait(0)==STATUS_SUCCESS);
        signaled.Reset();

        // A first signal retained by a failed WaitAll must not repeatedly wake
        // that wait. Only signaling its missing second event can complete it.
        ObservedEvent missing(false);
        signaled.Set();KObject* both[]={&signaled,&missing};
        auto all=std::async(std::launch::async,[&]{return WaitMultiple(both,2,false,1000);});
        AwaitScans(missing,1);
        before=g_dispatchGeneration;
        const unsigned scans=missing.scans.load();
        for(unsigned i=0;i<10000;++i)signaled.Set();
        CHECK(g_dispatchGeneration==before);
        CHECK(missing.scans==scans);
        CHECK(all.wait_for(0ms)==std::future_status::timeout);
        missing.Set();CHECK(all.get()==STATUS_SUCCESS);
        CHECK(signaled.Wait(0)==(manual?STATUS_SUCCESS:STATUS_TIMEOUT));
        CHECK(missing.Wait(0)==STATUS_TIMEOUT);

        // Repeated signal-before-sleep races through single/any/all waits. The
        // waiter publishes its scan while holding the dispatcher lock, so Set
        // races the transition into sleep rather than an arbitrary delay.
        for(unsigned kind=0;kind<3;++kind)for(unsigned i=0;i<64;++i) {
            ObservedEvent event(manual);KEvent companion(true,true);
            KObject* list[]={&event,&companion};
            auto wait=std::async(std::launch::async,[&] {
                if(kind==0)return event.Wait(1000);
                return WaitMultiple(list,kind==1?1:2,kind==1,1000);
            });
            AwaitScans(event,1);
            event.Set();event.Set();
            CHECK(wait.get()==STATUS_SUCCESS);
        }
    }

    // Overlapping arrays share one synchronization event: a signal cannot
    // satisfy both waits. A retained manual event must satisfy both instead.
    for(bool manual:{false,true})for(bool any:{false,true}) {
        KEvent left(true,!any),right(true,!any);
        ObservedEvent shared(manual);
        KObject* a[]={&shared,&left};KObject* b[]={&right,&shared};
        auto wa=std::async(std::launch::async,[&]{return WaitMultiple(a,2,any,80);});
        auto wb=std::async(std::launch::async,[&]{return WaitMultiple(b,2,any,80);});
        AwaitScans(shared,2);shared.Set();
        auto ar=wa.get(),br=wb.get();
        if(manual)CHECK(ar==0 && br==(any?1u:0u));
        else CHECK((ar==0 && br==STATUS_TIMEOUT)||(ar==STATUS_TIMEOUT && br==(any?1u:0u)));
    }
    // Pulse must still wake current waiters even while a prior Set is retained.
    ObservedEvent pulse(true);KObject* one[]={&pulse};
    auto pw=std::async(std::launch::async,[&]{return WaitMultiple(one,1,true,1000);});
    AwaitScans(pulse,1);pulse.Pulse();CHECK(pw.get()==STATUS_SUCCESS);
    CHECK(pulse.Wait(0)==STATUS_TIMEOUT);
    std::printf("PASS %u checks: redundant-Set broadcast suppression, four producers, manual/auto single/any/all races, overlapping waits, pulse\n",checks);
}
'''


def main():
    objects=ROOT/'port/src/runtime/kernel/kernel_objects.cpp'
    production=objects.read_text(encoding='utf-8-sig')
    with tempfile.TemporaryDirectory(prefix='ttt2-event-set-') as temp:
        folder=Path(temp)
        (folder/'ppc_recomp_shared.h').write_text(SHARED,encoding='utf-8')
        includes=[folder,RECOMP/'XenonUtils',RECOMP/'thirdparty/simde',ROOT/'port/src/runtime',objects.parent]
        checker=CHECKER.replace('@OBJECTS@',objects.as_posix())
        exe=compile_one(folder,'event_set',checker,includes)
        subprocess.run([str(exe)],check=True,timeout=20)
        # Remove only the optimized branch; the notification-count assertion
        # reproduces the old broadcast storm without relying on timing noise.
        assert production.count('if (state) return;')==1
        old=folder/'old_objects.cpp'
        old.write_text(production.replace('if (state) return;',''),encoding='utf-8')
        exe=compile_one(folder,'old_set',CHECKER.replace('@OBJECTS@',old.as_posix()),includes)
        run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
        assert run.returncode and 'g_dispatchGeneration-before==1' in run.stderr,run.stdout+run.stderr
        print('PASS old repeated-broadcast negative control detected (10,001 notifications instead of one)')


if __name__=='__main__':main()
