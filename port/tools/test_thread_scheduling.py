"""Exercise real kernel thread objects and extracted production scheduling imports.

Uses bounded real Windows threads; no game data or running game is needed. Covers
startup/self suspend counts, resumes, affinity (guest bookkeeping always; host pinning only
with TTT2_THREAD_AFFINITY=1 - both modes run), priority application, guest previous-value
outputs, current-thread aliases, rejected targets and masks.
"""
import os

from pathlib import Path
import re
import subprocess
import tempfile

from test_kernel_waits import SHARED, RECOMP, ROOT
from test_vcmpgtuh import compile_one


CHECKER = r'''
#include "@OBJECTS@"
#include <future>
#include <vector>
bool WTrace() { return false; }
bool TryCloseGuestFile(uint32_t) { return false; }
bool TryCloseContentEnumerator(uint32_t, uint32_t&) { return false; }
static void Reporter(PPCContext&, uint8_t*) { std::abort(); }
PPCFunc* const g_unmappedReporter = Reporter;
@IMPORTS@
using namespace std::chrono_literals;
static std::atomic<unsigned> checks{0};
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); std::abort(); } } while(0)
template<class F> void Await(F pred) {
    const auto end = std::chrono::steady_clock::now()+2s;
    while (!pred() && std::chrono::steady_clock::now()<end) std::this_thread::yield();
    CHECK(pred());
}
static bool Parked(KThread* t, uint32_t count) {
    std::lock_guard lk(t->sm);
    return t->waitingForResume && t->suspendCount==count;
}
static void CheckWord(uint8_t* base, uint32_t ptr, uint32_t expected) {
    CHECK(GRead32(base,ptr)==expected);
    CHECK(GRead32(base,ptr-4)==0xA5A5A5A5);
    CHECK(GRead32(base,ptr+4)==0xA5A5A5A5);
}
int main() {
    CHECK(MapGuestCpu(1,0xA0)==0x20);
    CHECK(MapGuestCpu(2,0xA0)==0x80);
    CHECK(MapGuestCpu(4,0xA0)==0x20);
    CHECK(MapGuestCpu(32,0xA0)==0x80);
    CHECK(MapGuestCpu(1,0)==0);
    CHECK(MapGuestCpu(0,0xFF)==0);
    CHECK(MapGuestCpu(3,0xFF)==0);
    CHECK(MapGuestCpu(64,0xFF)==0);
    DWORD_PTR allowed=0, system=0;
    CHECK(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system) && allowed);
    const DWORD_PTR workerCpu = MapGuestCpu(2,allowed);
    CHECK(workerCpu && !(workerCpu & ~allowed));
    const char* pinEnv = std::getenv("TTT2_THREAD_AFFINITY");
    const bool pinHost = pinEnv && *pinEnv == '1';

    alignas(16) uint8_t memory[4096]; std::memset(memory,0xA5,sizeof(memory));
    uint8_t* base=memory;
    auto* worker = static_cast<KThread*>(KernelCreateThreadObject());
    const uint32_t handle=KernelObjectHandle(worker);
    volatile uint8_t cpu=0xFF;
    std::atomic<int> phase{0};
    std::atomic<bool> finish{false};
    worker->Start([&] {
        KernelBindCurrentThreadCpu(&cpu);
        CHECK(cpu==1);
        if (pinHost) CHECK((DWORD_PTR(1)<<GetCurrentProcessorNumber())==workerCpu);
        CHECK(GetThreadPriority(GetCurrentThread())==THREAD_PRIORITY_ABOVE_NORMAL);
        CHECK(KernelQueryThreadPriority(0xFFFFFFFE)==1);
        phase=1;
        PPCContext c{}; c.r3.u32=handle; c.r4.u32=0x100;
        __imp__NtSuspendThread(c,base);
        CHECK(c.r3.u32==STATUS_SUCCESS);
        CheckWord(base,0x100,0);
        phase=2;
        c.r3.u32=0xFFFFFFFE; c.r4.u32=0x120;
        __imp__NtSuspendThread(c,base);
        CHECK(c.r3.u32==STATUS_SUCCESS);
        CheckWord(base,0x120,0);
        phase=3;
        while(!finish) std::this_thread::yield();
    },true);
    CHECK(Parked(worker,1));
    CHECK(phase==0);
    PPCContext c{}; c.r3.u32=handle; c.r4.u32=2; c.r5.u32=0x140;
    __imp__KeSetAffinityThread(c,base);
    CHECK(c.r3.u32==STATUS_SUCCESS); CheckWord(base,0x140,1);
    CHECK(worker->guestAffinity==2);
    // Host pinning only on request; otherwise the host thread keeps the process mask.
    CHECK(SetThreadAffinityMask(worker->hostThread,workerCpu)==(pinHost ? workerCpu : allowed));
    c.r3.u32=handle;c.r4.s32=1;__imp__KeSetBasePriorityThread(c,base);
    CHECK(c.r3.s32==0);
    CHECK(GetThreadPriority(worker->hostThread)==THREAD_PRIORITY_ABOVE_NORMAL);
    c.r3.u32=handle;__imp__KeQueryBasePriorityThread(c,base);CHECK(c.r3.s32==1);
    uint32_t previous=0xDEADBEEF;
    CHECK(KernelSuspendThread(handle,&previous)==STATUS_SUCCESS && previous==1);
    CHECK(Parked(worker,2));
    CHECK(KernelResumeThread(handle,&previous)==STATUS_SUCCESS && previous==2);
    CHECK(Parked(worker,1) && phase==0);
    CHECK(KernelResumeThread(handle,&previous)==STATUS_SUCCESS && previous==1);
    Await([&]{return phase==1 && Parked(worker,1);});
    CHECK(KernelSuspendThread(handle,&previous)==STATUS_SUCCESS && previous==1);
    CHECK(KernelResumeThread(handle,&previous)==STATUS_SUCCESS && previous==2);
    CHECK(Parked(worker,1) && phase==1);
    c.r3.u32=handle;c.r4.u32=0x160;__imp__NtResumeThread(c,base);
    CHECK(c.r3.u32==STATUS_SUCCESS);CheckWord(base,0x160,1);
    Await([&]{return phase==2 && Parked(worker,1);});
    c.r3.u32=handle;c.r4.u32=0;__imp__NtResumeThread(c,base);
    CHECK(c.r3.u32==STATUS_SUCCESS);
    Await([&]{return phase==3;});
    previous=0xDEADBEEF;
    CHECK(KernelSuspendThread(handle,&previous)==0xC0000002u && previous==0xDEADBEEF);
    CHECK(KernelResumeThread(handle,&previous)==STATUS_SUCCESS && previous==0);
    finish=true;
    CHECK(worker->Wait(2000)==STATUS_SUCCESS);

    // Count saturation on a startup-suspended thread does not wrap or wake it.
    auto* counted = static_cast<KThread*>(KernelCreateThreadObject());
    uint32_t countedHandle=KernelObjectHandle(counted);
    std::atomic<bool> ran{false}; counted->Start([&]{ran=true;},true);
    for(uint32_t i=1;i<127;++i)
        CHECK(KernelSuspendThread(countedHandle,&previous)==STATUS_SUCCESS && previous==i);
    CHECK(KernelSuspendThread(countedHandle,&previous)==0xC000004Au);
    CHECK(Parked(counted,127) && !ran);
    for(uint32_t i=127;i>0;--i)
        CHECK(KernelResumeThread(countedHandle,&previous)==STATUS_SUCCESS && previous==i);
    CHECK(counted->Wait(2000)==STATUS_SUCCESS && ran);

    // Bootstrap/current alias has an actual retained handle and bound KPCR byte.
    const DWORD_PTR originalMask=SetThreadAffinityMask(GetCurrentThread(),allowed);
    CHECK(originalMask!=0);
    const int originalPriority=GetThreadPriority(GetCurrentThread());
    c={};c.r13.u32=0x400;c.r3.u32=0xFFFFFFFE;c.r4.u32=32;c.r5.u32=0x180;
    __imp__KeSetAffinityThread(c,base);
    CHECK(c.r3.u32==STATUS_SUCCESS);CheckWord(base,0x180,1);CHECK(memory[0x50C]==5);
    c.r3.u32=0xFFFFFFFE;c.r4.u32=1;c.r5.u32=0;
    __imp__KeSetAffinityThread(c,base);CHECK(c.r3.u32==STATUS_SUCCESS && memory[0x50C]==0);
    c.r3.u32=0xFFFFFFFE;c.r4.s32=100;__imp__KeSetBasePriorityThread(c,base);
    CHECK(c.r3.s32==0 && GetThreadPriority(GetCurrentThread())==THREAD_PRIORITY_HIGHEST);
    c.r3.u32=0xFFFFFFFE;c.r4.s32=-100;__imp__KeSetBasePriorityThread(c,base);
    CHECK(c.r3.s32==100 && GetThreadPriority(GetCurrentThread())==THREAD_PRIORITY_LOWEST);
    c.r3.u32=0xFFFFFFFE;__imp__KeQueryBasePriorityThread(c,base);CHECK(c.r3.s32==-100);
    CHECK(KernelSetThreadPriority(0xFFFFFFFE,0)==-100);
    CHECK(SetThreadAffinityMask(GetCurrentThread(),originalMask)!=0);
    CHECK(SetThreadPriority(GetCurrentThread(),originalPriority));

    auto wrong=KernelObjectHandle(new KEvent(false,false));
    for(uint32_t invalid:{uint32_t(0xF1234567),wrong}) {
        previous=0xDEADBEEF;
        CHECK(KernelSetThreadAffinity(invalid,1,previous)==STATUS_INVALID_HANDLE && previous==0xDEADBEEF);
        CHECK(KernelSuspendThread(invalid,&previous)==STATUS_INVALID_HANDLE);
        CHECK(KernelResumeThread(invalid,&previous)==STATUS_INVALID_HANDLE);
        CHECK(KernelSetThreadPriority(invalid,1)==0);
        CHECK(KernelQueryThreadPriority(invalid)==0);
    }
    for(uint32_t invalid:{0u,3u,64u,0xFFFFFFFFu}) {
        c={};c.r3.u32=0xFFFFFFFE;c.r4.u32=invalid;c.r5.u32=0x200;
        __imp__KeSetAffinityThread(c,base);
        CHECK(c.r3.u32==STATUS_INVALID_PARAMETER);CheckWord(base,0x200,0xA5A5A5A5);
    }
    std::printf("PASS %u thread checks (host pinning %s): actual host scheduling, PPC outputs, counted startup/self suspension, resumes, rejected inputs\n",checks.load(),pinHost?"on":"off");
}
'''


def main():
    source = (ROOT / "port/src/runtime/kernel/kernel_thread.cpp").read_text()
    imports = []
    for name in ("NtResumeThread", "NtSuspendThread", "KeSetAffinityThread",
                 "KeSetBasePriorityThread", "KeQueryBasePriorityThread"):
        match = re.search(r"PPC_FUNC\(__imp__" + name + r"\)\n\{.*?\n\}", source, re.S)
        if not match:
            raise SystemExit("Missing production import " + name)
        imports.append(match.group())
    with tempfile.TemporaryDirectory(prefix="ttt2-thread-") as temp:
        folder = Path(temp)
        (folder / "ppc_recomp_shared.h").write_text(SHARED)
        objects = ROOT / "port/src/runtime/kernel/kernel_objects.cpp"
        checker = CHECKER.replace("@OBJECTS@", objects.as_posix()).replace("@IMPORTS@", "\n".join(imports))
        exe = compile_one(folder, "scheduling", checker, [folder, RECOMP / "XenonUtils",
            RECOMP / "thirdparty/simde", ROOT / "port/src/runtime"])
        for pin in ("0", "1"):
            subprocess.run([str(exe)], check=True, timeout=20, env=dict(os.environ, TTT2_THREAD_AFFINITY=pin))


if __name__ == "__main__":
    main()
