// Cached physical view mirror.
//
// The console maps its 512 MB of physical memory three times: 0xA0000000 (64 KB pages),
// 0xC0000000 (16 MB pages) and 0xE0000000 (4 KB pages), all over the same bytes - Xenia
// memory.cc builds its vA0000000 / vC0000000 / vE0000000 heaps on one physical heap. The
// port allocates every physical block in the A view (MmAllocatePhysicalMemoryEx) and the GPU
// addresses it there (PhysToGuest). TTT2's D3D runtime, however, gives READ-ONLY texture
// locks the cached alias 0xC0000000 + (address & 0x1FFFFFFF) (+0x1000 for E-view input; the
// tail of sub_827382F0). The character bake's CPU recompression (sub_82AD8A00) locks its
// 1024x1024 resolve at A7515000 that way and reads C7515000 - all zeros here, so every baked
// costume texture became all-black DXT blocks.
//
// The guest block is one private MEM_WRITE_WATCH allocation (the GPU backend's texture
// invalidation depends on GetWriteWatch), so the two views cannot share pages. The C view is
// instead a coherent copy filled on demand: it starts PAGE_NOACCESS; the first access to a
// 64 KB chunk has the GPU backend write back any resolve covering it, copies the A bytes in
// and opens the chunk. A resolve into the A view, or a frame swap, closes chunks again,
// first copying any page the CPU wrote through the C view back to the A view.
//
// Some CPU reads of resolves go straight through the A view: the customization screen's card of
// a new custom character (its thumbnail is compressed on the CPU from a resolve; every byte came
// out zero, a black card - MEASURED 2026-10-21: TTT2_GPU_HW_WRITEBACK=1 made it appear). So a
// GPU-only resolve's A-view pages are also PAGE_GUARD (PhysGuardResolve): the first CPU touch of
// one raises STATUS_GUARD_PAGE_VIOLATION - Windows lifts that page's guard - and GuardVeh writes
// the resolve back before the access is retried. Pages are guarded only while unflagged, so a
// resolve repeated every frame that nothing on the CPU reads costs no protection change. The
// backend's own touches (under its lock, Scatter's helper threads included) just open the page,
// so while it writes a resolve back the other guest threads fault and wait for it; it drops the
// rest of the guards after. Windows calls given guest memory (ReadFile, ReadProcessMemory) fail
// on a guard page instead of faulting, so their callers make the range coherent first.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "phys_mirror.h"
#include "gpu_render.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

uint32_t WatchGuestLrOf(DWORD tid);           // data_watch.cpp (TTT2_GUARD_WATCH)

namespace {
constexpr uint32_t VIEW_A = 0xA0000000u, VIEW_C = 0xC0000000u, VIEW_SIZE = 0x20000000u;
constexpr uint32_t CHUNK = 0x10000u, PAGE = 0x1000u, CHUNKS = VIEW_SIZE / CHUNK;

uint8_t* g_mirrorBase = nullptr;
std::mutex g_mirrorMutex;
std::vector<uint8_t> g_open;                 // per chunk: 1 = filled and accessible
std::vector<uint32_t> g_openList;            // open chunk indices (for InvalidateAll)
std::atomic<uint32_t> g_generation{ 0 };     // bumped by every invalidation
std::atomic<uint64_t> g_fills{ 0 }, g_writeBacks{ 0 };

uint8_t* CView(uint32_t chunk) { return g_mirrorBase + VIEW_C + size_t(chunk) * CHUNK; }
uint8_t* AView(uint32_t chunk) { return g_mirrorBase + VIEW_A + size_t(chunk) * CHUNK; }

// Copy A -> C page by page, opening each page only once it is about to be written, so another
// thread touching a later page of the chunk faults (and waits on the mutex) instead of seeing
// half-copied bytes. Caller holds g_mirrorMutex.
void Fill(uint32_t chunk)
{
    uint8_t* c = CView(chunk);
    const uint8_t* a = AView(chunk);
    DWORD old;
    for (uint32_t p = 0; p < CHUNK; p += PAGE)
    {
        VirtualProtect(c + p, PAGE, PAGE_READWRITE, &old);
        std::memcpy(c + p, a + p, PAGE);
    }
    ResetWriteWatch(c, CHUNK);               // only guest writes from here on count
    g_open[chunk] = 1;
    g_openList.push_back(chunk);
    g_fills.fetch_add(1, std::memory_order_relaxed);
}

// Caller holds g_mirrorMutex. Pages written through the C view go back to the A view first.
void Close(uint32_t chunk)
{
    uint8_t* c = CView(chunk);
    DWORD old;
    VirtualProtect(c, CHUNK, PAGE_READONLY, &old);   // writers now fault and wait on the mutex
    PVOID written[CHUNK / PAGE];
    ULONG_PTR count = CHUNK / PAGE;
    DWORD granularity = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, c, CHUNK, written, &count, &granularity) == 0)
        for (ULONG_PTR i = 0; i < count; ++i)
        {
            const size_t offset = static_cast<uint8_t*>(written[i]) - c;
            std::memcpy(AView(chunk) + offset, c + offset, PAGE);
            g_writeBacks.fetch_add(1, std::memory_order_relaxed);
        }
    VirtualProtect(c, CHUNK, PAGE_NOACCESS, &old);
    g_open[chunk] = 0;
}

LONG CALLBACK MirrorVeh(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || !g_mirrorBase)
        return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t lo = reinterpret_cast<uint64_t>(g_mirrorBase) + VIEW_C;
    const uint64_t fault = r->ExceptionInformation[1];
    if (fault < lo || fault >= lo + VIEW_SIZE) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t chunk = uint32_t((fault - lo) / CHUNK);
    for (int attempt = 0;; ++attempt)
    {
        // The writeback takes the GPU backend lock; never hold the mirror lock across it
        // (the command processor invalidates with the backend idle but may hold it later).
        const uint32_t generation = g_generation.load(std::memory_order_acquire);
        GpuHwWriteBackRange(VIEW_A + chunk * CHUNK, CHUNK);
        std::lock_guard<std::mutex> lock(g_mirrorMutex);
        if (g_open[chunk]) return EXCEPTION_CONTINUE_EXECUTION;    // filled meanwhile; retry
        // A resolve landed between the writeback and the lock: write that one back too.
        if (g_generation.load(std::memory_order_acquire) != generation && attempt < 4) continue;
        Fill(chunk);
        // TTT2_MIRROR_LOG=1: name the code reading the cached view (host RVA -> rva2sym.py).
        static const bool log = [] { const char* e = std::getenv("TTT2_MIRROR_LOG"); return e && *e == '1'; }();
        static std::atomic<uint32_t> seen{ 0 };
        const uint32_t n = seen.fetch_add(1, std::memory_order_relaxed);
        if (log && (n < 400 || (n & 127) == 0))          // all early fills, then a sample
        {
            const uint64_t module = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
            // Unwind a few host frames from the fault: the callers are recompiled guest
            // functions (a guest memcpy alone does not say who copies).
            uint64_t callers[3] = {};
            CONTEXT walk = *ep->ContextRecord;
            for (uint64_t& caller : callers)
            {
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(walk.Rip, &imageBase, nullptr);
                if (!fn)
                {
                    // Leaf function (no unwind data): the return address is on top of the stack.
                    walk.Rip = *reinterpret_cast<const DWORD64*>(walk.Rsp);
                    walk.Rsp += 8;
                }
                else
                {
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, walk.Rip, fn, &walk, &handlerData, &establisher, nullptr);
                }
                if (!walk.Rip) break;
                caller = walk.Rip - module;
            }
            std::fprintf(stderr, "[phys-mirror] fill %08X %s pc RVA=0x%llX callers 0x%llX 0x%llX 0x%llX\n",
                         VIEW_C + chunk * CHUNK, r->ExceptionInformation[0] ? "write" : "read",
                         (unsigned long long)(reinterpret_cast<uint64_t>(r->ExceptionAddress) - module),
                         (unsigned long long)callers[0], (unsigned long long)callers[1], (unsigned long long)callers[2]);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
}

// ---- A-view guards (phys_mirror.h) -----------------------------------------------------------
constexpr uint32_t A_PAGES = VIEW_SIZE / PAGE;
std::mutex g_guardMutex;
std::vector<uint8_t> g_guarded;              // per A-view page: 1 = PAGE_GUARD set by us
std::atomic<uint32_t> g_guardedPages{ 0 };
std::atomic<uint64_t> g_guardFaults{ 0 };

bool GuardsOn()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_RESOLVE_GUARD"); return !(e && *e == '0'); }();
    return on && g_mirrorBase;
}

// The A-view pages [first, end) a guest range touches; false outside the A view.
bool PageRange(uint32_t start, uint32_t size, uint32_t& first, uint32_t& end)
{
    if (!size || start < VIEW_A || start >= VIEW_A + VIEW_SIZE) return false;
    const uint64_t phys = start - VIEW_A;
    first = uint32_t(phys / PAGE);
    end = uint32_t((std::min<uint64_t>)((phys + size + PAGE - 1) / PAGE, A_PAGES));
    return first < end;
}

// Pages [first, end) whose flag differs from `guard` get the protection and the flag, in runs.
// Caller holds g_guardMutex.
void SetGuards(uint32_t first, uint32_t end, bool guard)
{
    DWORD old;
    for (uint32_t p = first; p < end;)
    {
        if (g_guarded[p] == uint8_t(guard)) { ++p; continue; }
        uint32_t q = p;
        while (q < end && g_guarded[q] != uint8_t(guard)) g_guarded[q++] = uint8_t(guard);
        VirtualProtect(g_mirrorBase + VIEW_A + size_t(p) * PAGE, size_t(q - p) * PAGE,
                       guard ? PAGE_READWRITE | PAGE_GUARD : PAGE_READWRITE, &old);
        if (guard) g_guardedPages.fetch_add(q - p, std::memory_order_relaxed);
        else g_guardedPages.fetch_sub(q - p, std::memory_order_relaxed);
        p = q;
    }
}

// TTT2_GUARD_WATCH=<hex start>:<hex size>[,...] (diagnostics): every CPU access to those A-view
// pages is logged as a [guard-watch] line (address, read / write, host RVA -> port/tools/rva2sym.py,
// guest LR) - each new (RVA, LR, kind) once, then a count every 4096 hits. The page's guard is
// re-armed after the access by single-stepping it.
std::vector<uint8_t> g_watch;                // per A-view page: watched
// Watched pages to re-guard after the single step. A stack: a writeback inside the handler can
// touch watched pages too (a nested fault and step) before the outer instruction completes.
thread_local uint32_t tl_rearm[16];
thread_local int tl_rearmCount = 0;

void ParseWatch()
{
    const char* e = std::getenv("TTT2_GUARD_WATCH");
    if (!e || !*e) return;
    g_watch.assign(A_PAGES, 0);
    const std::string all(e);
    for (size_t at = 0; at < all.size();)
    {
        const size_t comma = (std::min)(all.find(',', at), all.size());
        const std::string item = all.substr(at, comma - at);
        at = comma + 1;
        const size_t colon = item.find(':');
        const uint32_t start = uint32_t(std::strtoul(item.c_str(), nullptr, 16));
        const uint32_t size = colon == std::string::npos ? PAGE : uint32_t(std::strtoul(item.c_str() + colon + 1, nullptr, 16));
        if (start < VIEW_A || start >= VIEW_A + VIEW_SIZE || !size) continue;
        const uint32_t first = (start - VIEW_A) / PAGE;
        const uint32_t last = (std::min)(A_PAGES, uint32_t((uint64_t(start - VIEW_A) + size + PAGE - 1) / PAGE));
        DWORD old;
        for (uint32_t q = first; q < last; ++q)
        {
            g_watch[q] = 1;
            VirtualProtect(g_mirrorBase + VIEW_A + size_t(q) * PAGE, PAGE, PAGE_READWRITE | PAGE_GUARD, &old);
        }
        std::fprintf(stderr, "[guard-watch] watching %08X..%08X\n", VIEW_A + first * PAGE, VIEW_A + last * PAGE);
    }
}

void WatchHit(const EXCEPTION_POINTERS* ep, uint32_t address)
{
    static std::mutex m;
    static std::map<std::tuple<uint64_t, uint32_t, int>, uint64_t> seen;
    static uint64_t hits = 0;
    const uint64_t module = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    const uint64_t rva = reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress) - module;
    const int kind = int(ep->ExceptionRecord->ExceptionInformation[0]);    // 0 read, 1 write
    const uint32_t lr = WatchGuestLrOf(GetCurrentThreadId());
    std::lock_guard<std::mutex> lock(m);
    const uint64_t n = ++seen[{ rva, lr, kind }];
    ++hits;
    if (n == 1 && seen.size() <= 400)
    {
        uint64_t callers[3] = {};
        CONTEXT walk = *ep->ContextRecord;
        for (uint64_t& caller : callers)
        {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(walk.Rip, &imageBase, nullptr);
            if (!fn) { walk.Rip = *reinterpret_cast<const DWORD64*>(walk.Rsp); walk.Rsp += 8; }
            else
            {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, walk.Rip, fn, &walk, &handlerData, &establisher, nullptr);
            }
            if (!walk.Rip) break;
            caller = walk.Rip - module;
        }
        std::fprintf(stderr, "[guard-watch] %s %08X pc RVA=0x%llX guest LR=%08X tid %lu callers 0x%llX 0x%llX 0x%llX\n",
                     kind ? "write" : "read", address, (unsigned long long)rva, lr, GetCurrentThreadId(),
                     (unsigned long long)callers[0], (unsigned long long)callers[1], (unsigned long long)callers[2]);
    }
    if ((hits & 4095) == 0) std::fprintf(stderr, "[guard-watch] %llu hits\n", (unsigned long long)hits);
}

LONG CALLBACK GuardVeh(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && tl_rearmCount)
    {
        DWORD old;                                       // the watched accesses are done: arm them again
        for (int i = 0; i < tl_rearmCount; ++i)
            VirtualProtect(g_mirrorBase + VIEW_A + size_t(tl_rearm[i]) * PAGE, PAGE, PAGE_READWRITE | PAGE_GUARD, &old);
        tl_rearmCount = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION || r->NumberParameters < 2 || !g_mirrorBase)
        return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t lo = reinterpret_cast<uint64_t>(g_mirrorBase) + VIEW_A;
    const uint64_t fault = r->ExceptionInformation[1];
    if (fault < lo || fault >= lo + VIEW_SIZE) return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t page = uint32_t((fault - lo) / PAGE);
    if (!g_watch.empty() && g_watch[page])
    {
        WatchHit(ep, VIEW_A + uint32_t(fault - lo));
        if (tl_rearmCount < 16) tl_rearm[tl_rearmCount++] = page;
        ep->ContextRecord->EFlags |= 0x100;              // trap after this one instruction
    }
    {
        // Windows has already lifted this page's guard.
        std::lock_guard<std::mutex> lock(g_guardMutex);
        if (!g_guarded[page]) return EXCEPTION_CONTINUE_EXECUTION;   // another thread's fault got it
        g_guarded[page] = 0;
        g_guardedPages.fetch_sub(1, std::memory_order_relaxed);
    }
    const uint64_t n = g_guardFaults.fetch_add(1, std::memory_order_relaxed);
    // The GPU backend's own reads (texture decodes under its lock) keep today's behaviour: the
    // page opens without a writeback, so the backend never waits on itself.
    if (GpuHwThreadInBackend()) return EXCEPTION_CONTINUE_EXECUTION;
    GpuHwWriteBackRange(VIEW_A + page * PAGE, PAGE, true);   // the resolve(s) on this page, whole
    static const bool trace = [] { const char* e = std::getenv("TTT2_RESOLVE_TRACE"); return e && *e == '1'; }();
    if (n < 40 || (n & 1023) == 0 || trace)
    {
        const uint64_t module = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        std::fprintf(stderr, "[phys-guard] cpu %s of resolved page %08X (fault #%llu, pc RVA=0x%llX)\n",
                     r->ExceptionInformation[0] ? "write" : "read", VIEW_A + page * PAGE, (unsigned long long)n + 1,
                     (unsigned long long)(reinterpret_cast<uint64_t>(r->ExceptionAddress) - module));
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
}

void PhysGuardResolve(uint32_t start, uint32_t size)
{
    uint32_t first, end;
    if (!GuardsOn() || !PageRange(start, size, first, end)) return;
    std::lock_guard<std::mutex> lock(g_guardMutex);
    SetGuards(first, end, true);
}

void PhysUnguard(uint32_t start, uint32_t size)
{
    uint32_t first, end;
    if (!GuardsOn() || !g_guardedPages.load(std::memory_order_relaxed) || !PageRange(start, size, first, end)) return;
    std::lock_guard<std::mutex> lock(g_guardMutex);
    SetGuards(first, end, false);
}

void PhysPrepareHostAccess(uint32_t start, uint32_t size)
{
    uint32_t first, end;
    if (!GuardsOn() || !g_guardedPages.load(std::memory_order_relaxed) || !PageRange(start, size, first, end)) return;
    bool any = false;
    {
        std::lock_guard<std::mutex> lock(g_guardMutex);
        for (uint32_t p = first; p < end && !any; ++p) any = g_guarded[p] != 0;
    }
    if (!any) return;
    if (!GpuHwThreadInBackend()) GpuHwWriteBackRange(start, size);   // writes back and unguards
    PhysUnguard(start, size);                                         // what no live resolve covers
}

void PhysMirrorInit(uint8_t* base)
{
    g_open.assign(CHUNKS, 0);
    g_mirrorBase = base;
    DWORD old;
    if (!VirtualProtect(base + VIEW_C, VIEW_SIZE, PAGE_NOACCESS, &old))
    {
        std::fprintf(stderr, "[phys-mirror] cannot close the cached view (%lu); C-view reads stay zero\n",
                     GetLastError());
        g_mirrorBase = nullptr;
        return;
    }
    AddVectoredExceptionHandler(1, MirrorVeh);
    g_guarded.assign(A_PAGES, 0);
    ParseWatch();
    AddVectoredExceptionHandler(1, GuardVeh);
    std::fprintf(stderr, "[phys-guard] A-view resolve guards %s\n", GuardsOn() ? "on" : "off (TTT2_RESOLVE_GUARD=0)");
}

void PhysMirrorInvalidate(uint32_t start, uint32_t size)
{
    if (!g_mirrorBase || !size) return;
    // Accept either view's address for the same physical bytes.
    const uint32_t phys = start & 0x1FFFFFFFu;
    if (start < VIEW_A || start >= VIEW_C + VIEW_SIZE) return;
    const uint64_t end = (std::min<uint64_t>)(uint64_t(phys) + size, VIEW_SIZE);
    std::lock_guard<std::mutex> lock(g_mirrorMutex);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    bool closed = false;
    for (uint32_t chunk = phys / CHUNK; uint64_t(chunk) * CHUNK < end; ++chunk)
        if (g_open[chunk]) { Close(chunk); closed = true; }
    if (closed)
    {
        std::vector<uint32_t> still;
        for (uint32_t chunk : g_openList) if (g_open[chunk]) still.push_back(chunk);
        g_openList.swap(still);
    }
}

void PhysMirrorInvalidateAll()
{
    if (!g_mirrorBase) return;
    std::lock_guard<std::mutex> lock(g_mirrorMutex);
    g_generation.fetch_add(1, std::memory_order_acq_rel);
    for (uint32_t chunk : g_openList) if (g_open[chunk]) Close(chunk);
    g_openList.clear();
    static uint64_t reported = 0;
    const uint64_t fills = g_fills.load(std::memory_order_relaxed);
    if (fills >= reported + 256)              // occasional activity line, not per frame
    {
        reported = fills;
        std::fprintf(stderr, "[phys-mirror] cached-view chunk fills=%llu page writebacks=%llu\n",
                     (unsigned long long)fills, (unsigned long long)g_writeBacks.load());
    }
}
