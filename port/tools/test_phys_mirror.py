"""Cached physical view (0xC0000000) mirror regression; no game image or full build required.

Compiles the production phys_mirror.cpp against a MEM_WRITE_WATCH guest block laid out like
main.cpp's and a stand-in GPU backend holding one GPU-only resolve. Checks that C-view reads
see A-view bytes (including GPU-only bytes, written back on first touch), that invalidation
refreshes them, that pages written through the C view return to the A view without
clobbering other pages, and that unrelated access violations still reach SEH.
Negative control: the same harness without PhysMirrorInit must fail (C reads are zero, the
bug that blacked out every baked costume texture).
"""

from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one


ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / "port/src/runtime/kernel"

HARNESS = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
#include "phys_mirror.h"
#include "gpu_render.h"
#include "phys_mirror.cpp"

static uint8_t* g_b = nullptr;
static bool g_gpuOnly = false;                 // a resolve at A0100000..A0120000 not yet in memory
static std::vector<std::pair<uint32_t, uint32_t>> g_writeBackCalls;
void GpuHwWriteBackRange(uint32_t start, uint32_t size)
{
    g_writeBackCalls.push_back({ start, size });
    if (g_gpuOnly && start < 0xA0120000u && 0xA0100000u < start + size)
    {
        std::memset(g_b + 0xA0100000u, 0x5A, 0x20000);
        g_gpuOnly = false;
    }
}

static unsigned checks = 0, failures = 0;
#define CHECK(cond, what) do { if (cond) ++checks; else { ++failures; std::fprintf(stderr, "FAIL %s\n", what); } } while (0)

// The fault handler's side effects are invisible to the optimiser: keep every access in
// program order (volatile + signal fences), as each guest load/store is in the real runtime.
static uint32_t Read32(uint32_t ga)
{
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const uint32_t v = *reinterpret_cast<volatile uint32_t*>(g_b + ga);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    return v;
}
static void Write32(uint32_t ga, uint32_t v)
{
    std::atomic_signal_fence(std::memory_order_seq_cst);
    *reinterpret_cast<volatile uint32_t*>(g_b + ga) = v;
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

// clang-cl SEH only sees faults raised inside a call made from the __try body.
__declspec(noinline) static void Poke(uint8_t* page) { *reinterpret_cast<volatile uint32_t*>(page) = 1; }
static bool UnrelatedFaultReachesSeh(uint8_t* page)
{
    __try { Poke(page); return false; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

int main()
{
    g_b = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE));
    if (!g_b || !VirtualAlloc(g_b + 0x10000000u, 0x1000, MEM_COMMIT, PAGE_NOACCESS) ||
        !VirtualAlloc(g_b + 0xA0000000u, 0x40000000u, MEM_COMMIT, PAGE_READWRITE))
    { std::fprintf(stderr, "FAIL guest block allocation (%lu)\n", GetLastError()); return 2; }
#ifndef NO_MIRROR
    PhysMirrorInit(g_b);
#endif

    // 1. CPU bytes in the A view are visible through the C view, also across a chunk edge.
    Write32(0xA0200000u, 0x11223344u);
    Write32(0xA020FFFCu, 0xAAAAAAAAu); Write32(0xA0210000u, 0xBBBBBBBBu);
    CHECK(Read32(0xC0200000u) == 0x11223344u, "C view reads A bytes");
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const uint64_t straddle = *reinterpret_cast<volatile uint64_t*>(g_b + 0xC020FFFCu);
    CHECK(straddle == 0xBBBBBBBBAAAAAAAAull, "read straddling two chunks");

    // 2. GPU-only resolve bytes are written back on the first C-view touch of their chunk.
    g_gpuOnly = true;
    g_writeBackCalls.clear();
    CHECK((Read32(0xC0101234u) & 0xFF) == 0x5A, "GPU-only resolve bytes reach the C view");
    bool asked = false;
    for (auto& [s, n] : g_writeBackCalls) asked |= s <= 0xA0101234u && 0xA0101234u < s + n;
    CHECK(asked, "backend asked to write back the touched range");

    // 3. Invalidation (a later resolve into the A view) refreshes the C copy.
    Write32(0xA0200000u, 0x55667788u);
    PhysMirrorInvalidate(0xA0200000u, 0x100);
    CHECK(Read32(0xC0200000u) == 0x55667788u, "invalidate refreshes the C view");

    // 4. A page written through the C view returns to the A view at the next close; a page
    //    of the same chunk only the A view changed is not clobbered.
    CHECK(Read32(0xC0300010u) == 0, "fresh chunk reads zero");
    Write32(0xC0300010u, 0xCAFEF00Du);
    Write32(0xA0302000u, 0x0BADF00Du);
    PhysMirrorInvalidateAll();
    CHECK(Read32(0xA0300010u) == 0xCAFEF00Du, "C-view write copied back to the A view");
    CHECK(Read32(0xA0302000u) == 0x0BADF00Du, "untouched C page does not clobber A");
    CHECK(Read32(0xC0302000u) == 0x0BADF00Du, "reopened chunk sees the A write");

    // 5. Access violations outside the C view are not swallowed.
    CHECK(UnrelatedFaultReachesSeh(g_b + 0x10000000u), "unrelated fault still reaches SEH");

    if (failures) { std::fprintf(stderr, "%u of %u checks failed\n", failures, checks + failures); return 1; }
    std::printf("PASS %u cached-view checks: A->C coherence, chunk straddle, GPU-only writeback, invalidate, C->A write-back, unrelated faults\n", checks);
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="ttt2-physmirror-") as temp:
        folder = Path(temp)
        exe = compile_one(folder, "mirror", HARNESS, [KERNEL])
        subprocess.run([str(exe)], check=True, timeout=60)
        old = compile_one(folder, "mirror_old", "#define NO_MIRROR\n" + HARNESS, [KERNEL])
        result = subprocess.run([str(old)], capture_output=True, text=True, timeout=60)
        if result.returncode != 1 or "FAIL C view reads A bytes" not in result.stderr:
            raise AssertionError(f"harness without the mirror unexpectedly passed: {result.returncode}\n{result.stderr}")
        print("PASS negative control: without the mirror the C view reads zero")


if __name__ == "__main__":
    main()
