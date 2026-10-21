"""Compile and exercise the host-only function lookup with both available headers.

No game data is required. Uses the same clang-cl/Visual Studio setup as the other
runtime regression scripts. The simulated old table overwrite is guest memory;
host callback pointers must survive it.
"""

from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one


ROOT = Path(__file__).resolve().parents[2]
RECOMP = ROOT / "tools/XenonRecomp"

CHECKER = r'''
#define PPC_CONFIG_H_INCLUDED
#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0x11C0000ull
#define PPC_CODE_BASE 0x82250000ull
#define PPC_CODE_SIZE 0xB6848Cull
#include <ppc_context.h>
#include <cassert>
#include <cstdio>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static unsigned reported=0, called=0;
static void Reporter(PPCContext&,uint8_t*) { ++reported; }
static void Callback(PPCContext&,uint8_t*) { ++called; }
PPCFunc* const g_unmappedReporter=Reporter;
int main() {
    PPCContext ctx{};
    PPCCallIndirectChecked(ctx,nullptr,uint32_t(PPC_CODE_BASE));
    assert(reported==1);
    std::vector<PPCFunc*> entries(PPC_CODE_SIZE/4);
    g_ppcFunctionTable=entries.data();
    const uint32_t first=uint32_t(PPC_CODE_BASE);
    const uint32_t last=first+uint32_t(PPC_CODE_SIZE)-4;
    const uint32_t sample=first+128;
    // Reserve guest addresses without committing 4 GiB of backing storage.
    auto* guest=static_cast<uint8_t*>(VirtualAlloc(nullptr,PPC_MEMORY_SIZE,MEM_RESERVE,PAGE_NOACCESS));
    assert(guest);
    for(uint32_t target:{first,last,sample}) {
        PPC_LOOKUP_FUNC(guest,target)=Callback;
        PPCCallIndirectChecked(ctx,guest,target);
    }
    assert(called==3);
    for(uint32_t target:{0u,first-4,first+1,first+uint32_t(PPC_CODE_SIZE),0xFFFFFFFFu})
        PPCCallIndirectChecked(ctx,guest,target);
    PPCCallIndirectChecked(ctx,guest,first+4); // Valid address, null host slot.
    assert(reported==7);
    const uint64_t oldOffset=PPC_IMAGE_BASE+PPC_IMAGE_SIZE+uint64_t(sample-first)*2;
    assert(VirtualAlloc(guest+(oldOffset&~uint64_t(0xFFF)),0x1000,MEM_COMMIT,PAGE_READWRITE));
    *reinterpret_cast<uint64_t*>(guest+oldOffset)=0x610BB63D610BB63Dull;
    PPCCallIndirectChecked(ctx,guest,sample);
    assert(called==4);
    PPC_LOOKUP_FUNC(guest,0)=Callback; // Invalid assignment cannot alter real slots.
    PPCCallIndirectChecked(ctx,guest,0);
    assert(reported==8 && called==4);
    VirtualFree(guest,0,MEM_RELEASE);
    g_ppcFunctionTable=nullptr;
    std::puts("PASS host lookup: range, alignment, null fallback, mutable slots, guest overwrite isolation");
}
'''


def main():
    headers = [RECOMP / "XenonUtils"]
    if (ROOT / "re/ppc/ppc_context.h").exists():
        headers.append(ROOT / "re/ppc")
    with tempfile.TemporaryDirectory(prefix="ttt2-lookup-") as temp:
        folder = Path(temp)
        for index, header in enumerate(headers):
            exe = compile_one(folder, f"lookup{index}", CHECKER,
                              [header, RECOMP / "thirdparty/simde"])
            print(f"Header: {header}", flush=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
