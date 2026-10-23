// Data watchpoints on guest memory (diagnostics): which recompiled function reads or writes a
// guest address. TTT2_DATAWATCH=1 starts a thread that polls C:/ttt2/port/datawatch.txt
// ("<guest address hex> [r|w] [<second address hex>]"; r = reads and writes, w = writes only); while
// it names an address, debug register DR0 (and DR1 for a second address) of every thread in the
// process watches those 4 bytes, and each
// hit's host instruction (an RVA inside a recompiled sub_XXXXXXXX: port/tools/rva2sym.py) and the
// guest LR of that thread are counted, printed once a second as "[datawatch]" lines.
#include <windows.h>
#include <tlhelp32.h>
#include "kernel_shared.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

PPCContext* WatchGuestContextOf(DWORD tid);    // diag_hooks.cpp

// The guest LR of a thread (0 when it runs no guest code): phys_mirror.cpp's TTT2_GUARD_WATCH.
uint32_t WatchGuestLrOf(DWORD tid)
{
    const PPCContext* c = WatchGuestContextOf(tid);
    return c ? uint32_t(c->lr) : 0;
}

namespace {
std::atomic<uint64_t> g_watchHost{ 0 };
std::mutex g_hitsMutex;
std::map<std::pair<uint64_t, uint32_t>, uint32_t> g_hits;   // (host RVA | slot << 40, guest LR) -> count

LONG CALLBACK WatchVeh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 3))
        return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t slot = (ep->ContextRecord->Dr6 & 2) ? 1 : 0;
    ep->ContextRecord->Dr6 = 0;
    const uint64_t rva = ep->ContextRecord->Rip - reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    PPCContext* c = WatchGuestContextOf(GetCurrentThreadId());
    const uint32_t lr = c ? uint32_t(c->lr) : 0;
    if (g_hitsMutex.try_lock())
    {
        ++g_hits[{ rva | (slot << 40), lr }];
        g_hitsMutex.unlock();
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void Arm(uint64_t host, bool writesOnly, uint64_t host2 = 0)
{
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{ sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != DWORD(-1))
        {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c))
            {
                c.Dr0 = host;
                c.Dr1 = host2;
                // L0 / L1 enable, RW = 01 write / 11 read-write, LEN = 11 (4 bytes)
                uint64_t d7 = c.Dr7 & ~0xFF000Full;
                const uint64_t rw = writesOnly ? 1ull : 3ull;
                if (host) d7 |= 1ull | (rw << 16) | (3ull << 18);
                if (host2) d7 |= 4ull | (rw << 20) | (3ull << 22);
                c.Dr7 = d7;
                SetThreadContext(t, &c);
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
}

void Loop()
{
    AddVectoredExceptionHandler(1, WatchVeh);
    std::string last;
    bool writesOnly = false;
    auto lastArm = std::chrono::steady_clock::now();
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        char text[64] = {};
        if (FILE* f = std::fopen("C:/ttt2/port/datawatch.txt", "r")) { std::fgets(text, sizeof(text), f); std::fclose(f); }
        const std::string now = text;
        const auto t = std::chrono::steady_clock::now();
        if (now != last || (g_watchHost && t - lastArm > std::chrono::seconds(2)))
        {
            uint32_t guest = 0, guest2 = 0;
            char mode = 'r';
            const int n = std::sscanf(text, "%x %c %x", &guest, &mode, &guest2);
            writesOnly = n >= 2 && mode == 'w';
            const uint64_t host = (n >= 1 && guest) ? reinterpret_cast<uint64_t>(g_base) + guest : 0;
            const uint64_t host2 = (n >= 3 && guest2) ? reinterpret_cast<uint64_t>(g_base) + guest2 : 0;
            if (now != last)
                std::fprintf(stderr, "[datawatch] %s guest %08X (%s)\n", host ? "watching" : "stopped", guest, writesOnly ? "writes" : "reads+writes");
            g_watchHost = host;
            Arm(host, writesOnly, host2);           // again every 2 s: threads started since
            last = now;
            lastArm = t;
        }
        std::map<std::pair<uint64_t, uint32_t>, uint32_t> hits;
        {
            std::lock_guard l(g_hitsMutex);
            hits.swap(g_hits);
        }
        for (const auto& [k, n] : hits)
            std::fprintf(stderr, "[datawatch] hit%s rva %llX guest lr %08X x%u\n", (k.first >> 40) ? " (2nd)" : "",
                         (unsigned long long)(k.first & 0xFFFFFFFFFFull), k.second, n);
    }
}

struct Start
{
    Start()
    {
        const char* e = std::getenv("TTT2_DATAWATCH");
        if (e && *e == '1') std::thread(Loop).detach();
    }
} g_start;
} // namespace
