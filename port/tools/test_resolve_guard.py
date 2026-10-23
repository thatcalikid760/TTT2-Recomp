"""Exercise the A-view resolve guards (phys_mirror.cpp) with the production source.

A GPU-only resolve's pages are PAGE_GUARD; the CPU's first touch writes the resolve back before the
access completes (the customization screen's black card for a new custom character). The GPU
backend is replaced by a mock write-back that fills the resolve with a pattern and drops its guards,
as Scatter does; the guard bookkeeping, the exception handler and the host-access path run
unchanged on a 4 GB reservation laid out like the game's guest space. Checks: a guest read and a
guest write see the written-back bytes, every page of the resolve is coherent after one fault, the
backend's own touches open a page without a writeback, an already guarded resolve is not
re-protected, ReadFile into a guarded buffer fails unless the host-access path ran first, freeing
drops the guards, four threads at once, and TTT2_RESOLVE_GUARD=0.
"""
from pathlib import Path
import os
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]

CHECKER = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
uint8_t* g_guest = nullptr;
std::atomic<int> writebacks{0};
thread_local volatile bool inBackend = false;   // read by the exception handler (as BackendScope)
uint32_t WatchGuestLrOf(DWORD) { return 0; }   // TTT2_GUARD_WATCH (diagnostics)
bool GpuHwThreadInBackend() { return inBackend; }
void PhysUnguard(uint32_t start, uint32_t size);
// The mock backend: one live resolve [resolveStart, +resolveSize) filled with 0x5A, like Scatter -
// under its lock, writing while the guards are up (its own touches open the pages), then the rest.
#include <mutex>
std::recursive_mutex backendLock;
uint32_t resolveStart = 0, resolveSize = 0;
void GpuHwWriteBackRange(uint32_t start, uint32_t size, bool = false) {
    std::lock_guard l(backendLock);
    if (!resolveSize || start >= resolveStart + resolveSize || start + size <= resolveStart) return;
    ++writebacks;
    inBackend = true;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    std::memset(g_guest + resolveStart, 0x5A, resolveSize);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    inBackend = false;
    PhysUnguard(resolveStart, resolveSize);
    resolveSize = 0;                                   // coherent now
}
@MIRROR@
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line%d: %s\n", __LINE__, #x); return 1; } } while (0)
bool Guarded(uint32_t addr) {
    MEMORY_BASIC_INFORMATION m{};
    VirtualQuery(g_guest + addr, &m, sizeof m);
    return (m.Protect & PAGE_GUARD) != 0;
}
volatile uint8_t sink;
int main(int argc, char** argv) {
    const bool off = argc > 1;
    g_guest = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS));
    CHECK(g_guest);
    CHECK(VirtualAlloc(g_guest + 0xA0000000u, 0x40000000u, MEM_COMMIT, PAGE_READWRITE));   // A + C views
    PhysMirrorInit(g_guest);
    const uint32_t res = 0xA7515000u, size = 0x30000;   // a 48-page resolve
    if (off && std::strcmp(argv[1], "watch") == 0) {
        // TTT2_GUARD_WATCH=A0200000:2000: each access is seen and the page is guarded again after it.
        CHECK(Guarded(0xA0200000u) && Guarded(0xA0201000u) && !Guarded(0xA0202000u));
        for (int i = 0; i < 3; ++i) { g_guest[0xA0200010u] = uint8_t(i); CHECK(Guarded(0xA0200000u)); }
        sink = g_guest[0xA0201020u];
        CHECK(Guarded(0xA0201000u) && g_guest[0xA0200010u] == 2 && writebacks == 0);
        std::printf("PASS %u guard-watch checks: logged, re-armed after every access\n", checks);
        return 0;
    }
    if (off) {
        PhysGuardResolve(res, size);
        CHECK(!Guarded(res));
        std::printf("PASS resolve guards off with TTT2_RESOLVE_GUARD=0\n");
        return 0;
    }
    // A guest read: written back first, then the read sees the resolve; the whole resolve is
    // coherent after that one fault.
    resolveStart = res; resolveSize = size;
    PhysGuardResolve(res, size);
    CHECK(Guarded(res) && Guarded(res + size - 1) && !Guarded(res + size));
    CHECK(g_guest[res + 0x1234] == 0x5A && writebacks == 1);
    CHECK(!Guarded(res) && !Guarded(res + size - 1));
    CHECK(g_guest[res + size - 1] == 0x5A && writebacks == 1);
    // A guest write: the resolve lands first, then the written byte.
    std::memset(g_guest + res, 0, size);
    resolveStart = res; resolveSize = size; writebacks = 0;
    PhysGuardResolve(res, size);
    g_guest[res + 0x2000] = 0x11;
    CHECK(writebacks == 1 && g_guest[res + 0x2000] == 0x11 && g_guest[res + 0x2001] == 0x5A);
    // Guarding a guarded resolve again changes nothing (a resolve every frame nobody reads).
    std::memset(g_guest + res, 0, size);
    resolveStart = res; resolveSize = size; writebacks = 0;
    PhysGuardResolve(res, size);
    PhysGuardResolve(res, size);
    CHECK(Guarded(res) && writebacks == 0);
    // The backend's own touch opens the page without a writeback (it must never wait on itself);
    // the other pages stay guarded.
    inBackend = true;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    sink = g_guest[res + 0x100];
    std::atomic_signal_fence(std::memory_order_seq_cst);
    inBackend = false;
    CHECK(writebacks == 0 && !Guarded(res) && Guarded(res + 0x1000));
    CHECK(g_guest[res + 0x1000] == 0x5A && writebacks == 1);   // a guest read still writes back
    // ReadFile into a guarded buffer fails; the host-access path makes it work.
    char path[MAX_PATH];
    GetTempPathA(MAX_PATH, path);
    std::strcat(path, "ttt2_resolve_guard.bin");
    {
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        std::vector<uint8_t> bytes(0x2000, 0x77);
        DWORD put = 0;
        WriteFile(f, bytes.data(), DWORD(bytes.size()), &put, nullptr);
        CloseHandle(f);
    }
    auto readInto = [&](uint32_t at) {
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        DWORD got = 0;
        const BOOL ok = ReadFile(f, g_guest + at, 0x2000, &got, nullptr);
        CloseHandle(f);
        return ok && got == 0x2000;
    };
    resolveStart = res; resolveSize = size; writebacks = 0;
    PhysUnguard(res, size);
    PhysGuardResolve(res, size);
    CHECK(!readInto(res + 0x4000));                            // why the callers prepare
    PhysUnguard(res, size);
    resolveStart = res; resolveSize = size;
    PhysGuardResolve(res, size);
    PhysPrepareHostAccess(res + 0x4000, 0x2000);
    CHECK(writebacks == 1 && readInto(res + 0x4000) && g_guest[res + 0x4000] == 0x77 && g_guest[res + 0x6000] == 0x5A);
    // A guard with no live resolve under it (freed, or the CPU overwrote the resolve): unguarded.
    resolveSize = 0;
    PhysGuardResolve(res, size);
    PhysUnguard(res, size);                                    // MmFreePhysicalMemory
    CHECK(!Guarded(res) && !Guarded(res + size - 1) && readInto(res));
    PhysGuardResolve(res, size);
    PhysPrepareHostAccess(res, 0x2000);
    CHECK(!Guarded(res) && !Guarded(res + 0x1000) && Guarded(res + 0x2000) && readInto(res));
    PhysUnguard(res, size);
    // Four guest threads touching different pages at once: one coherent resolve, no deadlock.
    std::memset(g_guest + res, 0, size);
    resolveStart = res; resolveSize = size; writebacks = 0;
    PhysGuardResolve(res, size);
    std::atomic<int> good{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] { if (g_guest[res + 0x1000 * (t * 3 + 1) + 7] == 0x5A) ++good; });
    for (auto& t : threads) t.join();
    CHECK(good == 4 && writebacks >= 1);
    std::printf("PASS %u resolve-guard checks: guest read / write write back first, one fault per resolve, backend touches, no re-protect, ReadFile, free, threads\n", checks);
    DeleteFileA(path);
}
'''


def main():
    kernel = ROOT / "port/src/runtime/kernel"
    mirror = (kernel / "phys_mirror.cpp").read_text(encoding="utf-8-sig")
    mirror = mirror.replace('#include "gpu_render.h"\n', "")
    with tempfile.TemporaryDirectory(prefix="ttt2-guard-") as temp:
        folder = Path(temp)
        exe = compile_one(folder, "guard", CHECKER.replace("@MIRROR@", mirror), [kernel])
        subprocess.run([str(exe)], check=True, timeout=60)
        subprocess.run([str(exe), "off"], check=True, timeout=60, env=dict(os.environ, TTT2_RESOLVE_GUARD="0"))
        r = subprocess.run([str(exe), "watch"], timeout=60, capture_output=True, text=True,
                           env=dict(os.environ, TTT2_GUARD_WATCH="A0200000:2000"))
        assert r.returncode == 0, r.stdout + r.stderr
        assert "[guard-watch] watching A0200000..A0202000" in r.stderr, r.stderr
        assert r.stderr.count("[guard-watch] write") >= 1 and r.stderr.count("[guard-watch] read") >= 1, r.stderr
        print(r.stdout, end="")


if __name__ == "__main__":
    main()
