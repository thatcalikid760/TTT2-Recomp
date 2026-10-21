// Opt-in diagnostic wrappers around individual recompiled game functions. The generated
// code defines every sub_X as a weak alias of __imp__sub_X, so a strong definition here
// takes over all direct and table calls while the original stays reachable as __imp__.
// Each wrapper only observes; it never changes arguments or results.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "kernel_shared.h"
#include "guest_heap.h"
#include "timeline.h"
#include <cmath>
#include <xmmintrin.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <string>
#include <chrono>
#include <map>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_82AD8A00);
extern "C" PPC_FUNC(__imp__sub_82BD3DF0);
extern "C" PPC_FUNC(__imp__sub_82BD43C0);
extern "C" PPC_FUNC(__imp__sub_82BD39B0);
extern "C" PPC_FUNC(__imp__sub_82BD4278);

// TTT2_TRACE_COPY=<text> (or hex:<bytes> for binary needles such as a XUID): log every call of
// the game's copy routines whose source starts with the needle - which routine, destination,
// size and the guest call chain - to find where a value (e.g. the default name "Player") is put
// into a structure. Routines: memcpy 82BD3DF0, strncpy 82BD43C0, memmove 82BD39B0, memcpy (2)
// 82BD4278; all take r3 dst, r4 src, r5 size.
static void TraceCopy(const char* routine, PPCContext& ctx, uint8_t* base)
{
    static std::string needleBytes = [] {
        const char* e = std::getenv("TTT2_TRACE_COPY");
        if (!e) return std::string();
        if (std::strncmp(e, "hex:", 4) != 0) return std::string(e);
        std::string b;
        for (const char* p = e + 4; p[0] && p[1]; p += 2) b.push_back(char(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
        return b;
    }();
    const size_t needleLen = needleBytes.size();
    if (!needleLen || ctx.r5.u32 < needleLen || ctx.r4.u32 < 0x1000 ||
        std::memcmp(base + ctx.r4.u32, needleBytes.data(), needleLen) != 0)
        return;
    static std::atomic<int> logged{ 0 };
    if (logged++ >= 20000) return;
    char line[400];
    int n = std::snprintf(line, sizeof(line), "[copy] %s '%s' %08X -> %08X (%u bytes) lr=%08X chain:", routine,
                          std::getenv("TTT2_TRACE_COPY"), ctx.r4.u32, ctx.r3.u32, ctx.r5.u32, uint32_t(ctx.lr));
    uint32_t frame = ctx.r1.u32;
    for (int i = 0; i < 10 && frame && frame < 0xFFFFFFF0u && n < 360; ++i)
    {
        const uint32_t prev = GRead32(base, frame);
        if (!prev || prev <= frame || prev - frame > 0x10000) break;
        n += std::snprintf(line + n, sizeof(line) - n, " %08X", GRead32(base, prev - 8));
        frame = prev;
    }
    std::fprintf(stderr, "%s\n", line);
}

PPC_FUNC(sub_82BD3DF0) { TraceCopy("memcpy", ctx, base); __imp__sub_82BD3DF0(ctx, base); }
PPC_FUNC(sub_82BD43C0) { TraceCopy("strncpy", ctx, base); __imp__sub_82BD43C0(ctx, base); }
PPC_FUNC(sub_82BD39B0) { TraceCopy("memmove", ctx, base); __imp__sub_82BD39B0(ctx, base); }
PPC_FUNC(sub_82BD4278) { TraceCopy("memcpy2", ctx, base); __imp__sub_82BD4278(ctx, base); }

namespace {
bool DiagBake()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_BAKE"); return e && *e == '1'; }();
    return on;
}

uint32_t NonZeroWords(uint8_t* base, uint32_t addr, uint32_t pitch, uint32_t rows, uint32_t rowBytes)
{
    uint32_t n = 0;
    for (uint32_t y = 0; y < rows; ++y)
        for (uint32_t x = 0; x < rowBytes; x += 4)
            n += *reinterpret_cast<uint32_t*>(base + addr + y * pitch + x) != 0;
    return n;
}
}

// Texture recompression dispatcher (CharacterTextureBake output -> DXTn). On entry:
// r4 destination bits, r5 source bits, r6 width, r7 height, r8 source pitch, r9 format.
PPC_FUNC(sub_82AD8A00)
{
    static uint32_t calls = 0;                     // guest calls are serialised per caller
    const uint32_t n = calls++;
    if (DiagBake() && (n < 64 || (n & 63) == 0))    // all early calls, then a sample
    {
        const uint32_t src = ctx.r5.u32, w = ctx.r6.u32, h = ctx.r7.u32, pitch = ctx.r8.u32;
        const uint32_t rows = h < 4096 ? h : 0, rowBytes = pitch < 0x4000 ? (w * 4 < pitch ? w * 4 : pitch) : 0;
        std::fprintf(stderr, "[diag-bake] recompress #%u dst=%08X src=%08X %ux%u pitch=%u fmt=%u r10=%08X "
            "src_nonzero=%u/%u first=%08X %08X\n", n, ctx.r4.u32, src, w, h, pitch, ctx.r9.u32, ctx.r10.u32,
            NonZeroWords(base, src, pitch, rows, rowBytes), rows * (rowBytes / 4),
            GRead32(base, src), GRead32(base, src + 4));
    }
    __imp__sub_82AD8A00(ctx, base);
}

// CPU skinning kernels (vtable slot 3 of the skinning job classes, table at 0x82232EA0): job
// r3 = {+4 palette, +8 palette, +12 bone base, +16 output, +20 input, +24 vertex count}; output
// stride per kernel (16..64 bytes, from each kernel's output increment). TTT2_DIAG_SKIN=1 logs jobs whose output range overlaps another recent job
// on a different input: the thin streak lines are vertices one job writes into another's mesh.
namespace {
struct SkinJob { uint32_t job, out, end, in, lr, tid; uint64_t seq; uint32_t count, stride, palA, palB, boneBase, boneCount, kernel; };
std::mutex g_skinMutex;
SkinJob g_skinRecent[512];
uint64_t g_skinSeq = 0;
int g_skinLogged = 0;

bool DiagSkin()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_DIAG_SKIN"); return e && *e == '1'; }();
    return on;
}

void NoteSkinJob(PPCContext& ctx, uint8_t* base, const char* kernel, uint32_t stride, uint32_t kernelAddr)
{
    const uint32_t job = ctx.r3.u32;
    const uint32_t count = GRead32(base, job + 24);
    const SkinJob cur{ job, GRead32(base, job + 16), GRead32(base, job + 16) + count * stride,
                       GRead32(base, job + 20), uint32_t(ctx.lr), GetCurrentThreadId(), 0,
                       count, stride, GRead32(base, job + 4), GRead32(base, job + 8),
                       GRead16(base, job + 12), GRead16(base, job + 14), kernelAddr };
    std::lock_guard lock(g_skinMutex);
    const uint64_t seq = ++g_skinSeq;
    for (const SkinJob& o : g_skinRecent)
    {
        if (!o.seq || seq - o.seq > 512 || g_skinLogged >= 80) continue;
        if (o.in != cur.in && o.out < cur.end && cur.out < o.end)
        {
            ++g_skinLogged;
            std::fprintf(stderr, "[diag-skin] OVERLAP %s job=%08X out=%08X..%08X in=%08X lr=%08X tid=%u"
                                 " | earlier(-%llu) job=%08X out=%08X..%08X in=%08X lr=%08X tid=%u\n",
                         kernel, cur.job, cur.out, cur.end, cur.in, cur.lr, cur.tid,
                         (unsigned long long)(seq - o.seq), o.job, o.out, o.end, o.in, o.lr, o.tid);
        }
    }
    SkinJob& slot = g_skinRecent[seq % 512];
    slot = cur;
    slot.seq = seq;
}
}

#define TTT2_SKIN_HOOK(addr, stride)                                                 \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                    \
    PPC_FUNC(sub_##addr)                                                       \
    {                                                                          \
        const timeline::Scope skinMark{ timeline::SkinJob, 0x##addr & 0xFFFFu };          \
        if (DiagSkin()) NoteSkinJob(ctx, base, #addr, stride, 0x##addr);                 \
        __imp__sub_##addr(ctx, base);                                          \
    }
TTT2_SKIN_HOOK(82B10800, 64)
TTT2_SKIN_HOOK(82B10908, 16)
TTT2_SKIN_HOOK(82B10A70, 32)
TTT2_SKIN_HOOK(82B10C88, 48)
TTT2_SKIN_HOOK(82B10F40, 64)
TTT2_SKIN_HOOK(82B11288, 20)
TTT2_SKIN_HOOK(82B11500, 36)
TTT2_SKIN_HOOK(82B118C0, 16)

// Dynamic vertex allocator (sub_82AD46F0 -> sub_82AD5B40: a plain cursor bump at this+24, no
// lock). TTT2_DIAG_SKIN=1 also reports two threads inside it at once (thread, guest CPU byte
// KPCR+0x10C, caller), which hands both callers the same output range.
extern "C" PPC_FUNC(__imp__sub_82AD46F0);
namespace {
std::atomic<int> g_allocInside{0};
std::atomic<uint32_t> g_allocOwner{0}, g_allocOwnerLr{0}, g_allocOwnerCpu{0};
std::atomic<int> g_allocLogged{0};
}
PPC_FUNC(sub_82AD46F0)
{
    if (!DiagSkin()) { __imp__sub_82AD46F0(ctx, base); return; }
    const uint32_t tid = GetCurrentThreadId(), lr = uint32_t(ctx.lr);
    const uint32_t cpu = ctx.r13.u32 ? base[ctx.r13.u32 + 0x10C] : 0xFF;
    if (g_allocInside.fetch_add(1) > 0 && g_allocLogged.fetch_add(1) < 40)
        std::fprintf(stderr, "[diag-skin] CONCURRENT alloc this=%08X size=%u tid=%u cpu=%u lr=%08X while tid=%u cpu=%u lr=%08X\n",
                     ctx.r3.u32, ctx.r4.u32, tid, cpu, lr, g_allocOwner.load(), g_allocOwnerCpu.load(), g_allocOwnerLr.load());
    g_allocOwner = tid; g_allocOwnerLr = lr; g_allocOwnerCpu = cpu;
    __imp__sub_82AD46F0(ctx, base);
    g_allocInside.fetch_sub(1);
}

// Skinned-vertex ring advance (sub_82AD5BA0: this+20 index mod this+68 regions, cursor = base).
// A region written in frame n is reused in frame n + regions; the GPU must have drawn it by
// then. TTT2_DIAG_SKIN=1 compares, at each advance, the game's swaps issued two advances
// earlier with the frames the command processor has presented: a positive deficit means the
// CPU is about to overwrite vertices the GPU emulation has not drawn yet.
extern "C" PPC_FUNC(__imp__sub_82AD5BA0);
extern std::atomic<uint64_t> g_cpPresents, g_gameSwaps;
namespace {
std::mutex g_ringMutex;
uint64_t g_ringSwaps[4] = {};
uint64_t g_ringAdvances = 0, g_ringDeficitHist[6] = {};
}
PPC_FUNC(sub_82AD5BA0)
{
    if (DiagSkin())
    {
        std::lock_guard lock(g_ringMutex);
        const uint64_t n = g_ringAdvances++;
        const uint64_t swapsNow = g_gameSwaps.load(), presented = g_cpPresents.load();
        if (n >= 2)
        {
            const uint64_t needed = g_ringSwaps[(n - 2) & 3];
            const int64_t deficit = int64_t(needed) - int64_t(presented);
            ++g_ringDeficitHist[deficit <= 0 ? 0 : deficit >= 5 ? 5 : deficit];
        }
        g_ringSwaps[n & 3] = swapsNow;
        if (n % 600 == 599)
            std::fprintf(stderr, "[diag-skin] ring advances=%llu swaps=%llu presented=%llu deficit hist 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu\n",
                         (unsigned long long)(n + 1), (unsigned long long)swapsNow, (unsigned long long)presented,
                         (unsigned long long)g_ringDeficitHist[0], (unsigned long long)g_ringDeficitHist[1],
                         (unsigned long long)g_ringDeficitHist[2], (unsigned long long)g_ringDeficitHist[3],
                         (unsigned long long)g_ringDeficitHist[4], (unsigned long long)g_ringDeficitHist[5]);
    }
    __imp__sub_82AD5BA0(ctx, base);
}

// Frame-log helper: describe the skinning job (most recent covering `addr`) and, for the
// 4-bone kernel (96-byte input: +64 four bone indices, +80 four weights), the source record
// of the vertex written at `addr`. Returns false when no recorded job covers it.
bool DiagSkinDescribe(uint32_t addr, char* out, size_t size)
{
    std::lock_guard lock(g_skinMutex);
    const SkinJob* best = nullptr;
    for (const SkinJob& j : g_skinRecent)
        if (j.seq && j.out <= addr && addr < j.end && (!best || j.seq > best->seq)) best = &j;
    if (!best) return false;
    uint8_t* base = g_base;
    const uint32_t stride = best->stride;
    const uint32_t vertex = stride ? (addr - best->out) / stride : 0;
    int n = std::snprintf(out, size, "job=%08X stride=%u vertex %u/%u bones [%u,+%u) palA=%08X palB=%08X in=%08X tid=%u",
                          best->job, stride, vertex, best->count, best->boneBase, best->boneCount,
                          best->palA, best->palB, best->in, best->tid);
    if (stride == 64 && n > 0 && size_t(n) < size)
    {
        const uint32_t rec = best->in + vertex * 96;
        float w[4];
        for (int k = 0; k < 4; ++k) { const uint32_t raw = GRead32(base, rec + 80 + k * 4); std::memcpy(&w[k], &raw, 4); }
        n += std::snprintf(out + n, size - n, " idx=%u,%u,%u,%u w=%.3g,%.3g,%.3g,%.3g",
                           GRead32(base, rec + 64), GRead32(base, rec + 68), GRead32(base, rec + 72), GRead32(base, rec + 76),
                           w[0], w[1], w[2], w[3]);
        // Re-run the game's 4-bone kernel now on this one vertex (same input record and
        // palettes, scratch output): same result = the inputs give this position; different =
        // the palettes changed after the job ran (it read them mid-update).
        if (best->kernel == 0x82B10F40u && n > 0 && size_t(n) < size)
        {
            static uint32_t scratch = 0;
            if (!scratch)
                if (void* p = g_virtualHeap.Alloc(0x11000, 0x100)) scratch = GuestAddr(base, p);
            if (scratch)
            {
                GWrite32(base, scratch + 4, best->palA);
                GWrite32(base, scratch + 8, best->palB);
                GWrite16(base, scratch + 12, uint16_t(best->boneBase));
                GWrite16(base, scratch + 14, uint16_t(best->boneCount));
                GWrite32(base, scratch + 16, scratch + 0x40);
                GWrite32(base, scratch + 20, rec);
                GWrite32(base, scratch + 24, 1);
                // Guest FP/VMX code loads MXCSR from the context; keep the host thread's own
                // (a zeroed context unmasks every FP exception: C000008F on the next op).
                const unsigned int hostCsr = _mm_getcsr();
                PPCContext c{};
                c.fpscr.csr = hostCsr;
                c.r3.u32 = scratch;
                c.r1.u32 = scratch + 0x11000 - 0x100;
                __imp__sub_82B10F40(c, base);
                _mm_setcsr(hostCsr);
                float a[3], b[3];
                for (int k = 0; k < 3; ++k)
                {
                    const uint32_t ra = GRead32(base, scratch + 0x40 + k * 4), rb = GRead32(base, addr + k * 4);
                    std::memcpy(&a[k], &ra, 4); std::memcpy(&b[k], &rb, 4);
                }
                const float d = std::fabs(a[0] - b[0]) + std::fabs(a[1] - b[1]) + std::fabs(a[2] - b[2]);
                std::snprintf(out + n, size - n, " rerun=(%.0f,%.0f,%.0f) %s", a[0], a[1], a[2], d < 1.0f ? "same" : "DIFF");
            }
        }
    }
    return true;
}

// Frame pacing (TTT2_FRAMETIME=1): how long the main thread waits for TTT2's job pool.
// sub_824F58B8(job) kicks a job of the pool threads (sub_824F5458, which poll for work with
// Sleep(1) while idle) and waits for it by entering the job's lock (+24). Every 10 s: waits,
// total and longest wait (ms).
extern "C" PPC_FUNC(__imp__sub_824F58B8);
PPC_FUNC(sub_824F58B8)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_FRAMETIME"); return e && *e == '1'; }();
    if (!on)
    {
        __imp__sub_824F58B8(ctx, base);
        return;
    }
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    __imp__sub_824F58B8(ctx, base);
    const double ms = std::chrono::duration<double, std::milli>(clock::now() - start).count();
    static std::mutex m;
    static uint64_t waits = 0;
    static double total = 0, longest = 0;
    static clock::time_point report = clock::now();
    std::lock_guard lock(m);
    ++waits;
    total += ms;
    longest = (std::max)(longest, ms);
    if (clock::now() - report >= std::chrono::seconds(10))
    {
        std::fprintf(stderr, "[frame] job-pool waits 10s: %llu waits, %.0f ms total, mean %.3f ms, longest %.1f ms\n",
                     (unsigned long long)waits, total, waits ? total / double(waits) : 0.0, longest);
        waits = 0;
        total = longest = 0;
        report = clock::now();
    }
}

// Frame timeline (TTT2_TIMELINE=1, timeline.h): the game-side waits of a frame.
// sub_82ACFF90(obj): main thread, polls obj+8 (busy) with 1 ms waits on the event at obj+196.
// sub_82AE4438(monitor, target): job worker, waits until the monitor's 64-bit count = target
// (marked in its replacement, guest_monitor.cpp).
// sub_82738DA0: D3D's wait for the GPU (loops on sub_8273D4A0).
extern "C" PPC_FUNC(__imp__sub_82ACFF90);
PPC_FUNC(sub_82ACFF90)
{
    const timeline::Scope mark{ timeline::MainResourceWait, ctx.r3.u32 };
    __imp__sub_82ACFF90(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_82738DA0);
PPC_FUNC(sub_82738DA0)
{
    const timeline::Scope mark{ timeline::MainGpuWait, ctx.r4.u32 };
    __imp__sub_82738DA0(ctx, base);
}

// TTT2_FRAMETIME=1: guest functions on the main thread's stack during the ~100 ms fight hitch
// (profile 2026-10-14) - each call over 8 ms is logged with its thread.
static void TimeCall(const char* name, PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*))
{
    static const bool on = [] { const char* e = std::getenv("TTT2_FRAMETIME"); return e && *e == '1'; }();
    if (!on) { fn(ctx, base); return; }
    const auto start = std::chrono::steady_clock::now();
    fn(ctx, base);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    static std::atomic<int> logged{ 0 };
    if (ms > 8.0 && logged++ < 400) std::fprintf(stderr, "[frame] slow call %s %.1f ms thread %lu\n", name, ms, GetCurrentThreadId());
}
#define TIMED_HOOK(addr) \
    extern "C" PPC_FUNC(__imp__sub_##addr); \
    PPC_FUNC(sub_##addr) { TimeCall("sub_" #addr, ctx, base, __imp__sub_##addr); }
TIMED_HOOK(82AD8F28)
TIMED_HOOK(82AFB798)
TIMED_HOOK(82B0BD68)
TIMED_HOOK(82C12250)
// TTT2_HITCH_PROFILE: an in-process sampler (profile_threads.py manages ~65 Hz from outside, too
// coarse for one 100 ms frame). A sampler thread suspends a thread, records its guest call chain
// (ctx.lr, then the r1 backchain: saved LR at [backchain - 8]) and its host stack (function starts
// via RtlVirtualUnwind; exe ones print as RVAs for port/tools/rva2sym.py, others as module+RVA).
//   1: the main thread every 0.5 ms while it runs a game task (sub_82A80AD8, hook below); a task
//      over 20 ms prints its profile.
//   2: also the main thread all the time (1 kHz), a profile per 10 s: where frame time goes.
//   3: flight recorder - the main and command-processor threads all the time (1 kHz), and for
//      every presented frame over 25 ms (HitchFrameGap) both threads' profiles over that gap.
//   4: the main and command-processor threads all the time (1 kHz), a profile of each per 10 s.
namespace hitch {
struct Sample { int64_t at; uint32_t chain[24]; uint32_t depth; uint64_t host[32]; uint32_t hostDepth; };
struct Target
{
    std::string name;
    DWORD tid = 0;
    HANDLE thread = nullptr;
    ULONG_PTR stackLow = 0, stackHigh = 0;
    PPCContext* ctx = nullptr;
    std::vector<Sample> ring;                     // flight recorder (mode 3), sampler thread only
    size_t next = 0;
    std::vector<Sample> window;                   // modes 2 and 4, sampler thread only
};
std::atomic<bool> g_active{ false }, g_busy{ false };
std::mutex g_targetsMutex, g_samplesMutex;
std::vector<Target*> g_targets;                   // [0] = main thread once known
std::atomic<Target*> g_main{ nullptr };
std::vector<Sample> g_samples;   // guest memory: the runtime's ::g_base (set before any thread)
struct Gap { int64_t end; double ms; };
std::mutex g_gapsMutex;
std::vector<Gap> g_gaps;
std::atomic<int> g_gapReports{ 0 };

int Mode()
{
    static const int mode = [] {
        const char* e = std::getenv("TTT2_HITCH_PROFILE");
        return e && *e >= '1' && *e <= '4' ? *e - '0' : 0;
    }();
    return mode;
}
bool On() { return Mode() != 0; }
void Report(const char* header, const std::vector<Sample>& samples, size_t guestTop = 40, size_t hostTop = 40, size_t leafTop = 20);
void Loop();

int64_t Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
int64_t Freq()
{
    static const int64_t f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return q.QuadPart; }();
    return f;
}

// Called on the thread to be sampled.
Target* Register(const char* name, PPCContext* ctx, bool main = false)
{
    auto* t = new Target;
    t->name = name;
    t->tid = GetCurrentThreadId();
    t->ctx = ctx;
    GetCurrentThreadStackLimits(&t->stackLow, &t->stackHigh);
    t->thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
    if (Mode() == 3) t->ring.resize(1024);
    static std::once_flag started;
    std::call_once(started, [] { std::thread(Loop).detach(); });
    if (main) g_main = t;
    std::lock_guard l(g_targetsMutex);
    g_targets.push_back(t);
    return t;
}

uint32_t FunctionOf(uint32_t address)
{
    static const size_t count = [] { size_t n = 0; while (PPCFuncMappings[n].guest) ++n; return n; }();
    size_t lo = 0, hi = count;
    while (lo < hi)
    {
        const size_t mid = (lo + hi) / 2;
        if (PPCFuncMappings[mid].guest <= address) lo = mid + 1;
        else hi = mid;
    }
    return lo ? PPCFuncMappings[lo - 1].guest : 0;
}

// kernel_thread.cpp's registry: host thread -> the PPCContext of its current guest call.
struct GuestContextSlot { uint32_t tid; uint32_t entry; PPCContext* ctx; };
extern "C" struct GuestContextRegistry
{
    uint32_t magic, version, r1Offset, lrOffset, capacity;
    std::atomic<uint32_t> count;
    GuestContextSlot slots[512];
} g_guestContextRegistry;

PPCContext* GuestContextOf(DWORD tid)
{
    const uint32_t n = (std::min)(g_guestContextRegistry.count.load(), 512u);
    for (uint32_t i = 0; i < n; ++i)
        if (g_guestContextRegistry.slots[i].tid == tid) return g_guestContextRegistry.slots[i].ctx;
    return nullptr;
}

// The walks run while the thread is suspended; a fault (an odd host frame, a stale guest
// context) loses the sample instead of the process.
bool Walk(Target& t, Sample& s);
bool Take(Target& t, Sample& s)
{
    if (SuspendThread(t.thread) == DWORD(-1)) return false;
    s = {};
    s.at = Now();
    const bool ok = Walk(t, s);
    ResumeThread(t.thread);
    return ok;
}

bool Walk(Target& t, Sample& s)
{
    __try
    {
    CONTEXT hc{};
    hc.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(t.thread, &hc))
    {
        // Leaf functions without unwind data return through [rsp].
        for (; s.hostDepth < 32 && hc.Rip && hc.Rsp >= t.stackLow && hc.Rsp + 8 <= t.stackHigh; )
        {
            DWORD64 image = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(hc.Rip, &image, nullptr);
            s.host[s.hostDepth++] = fn ? image + fn->BeginAddress : hc.Rip;
            if (!fn)
            {
                hc.Rip = *reinterpret_cast<DWORD64*>(hc.Rsp);
                hc.Rsp += 8;
                continue;
            }
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, hc.Rip, fn, &hc, &handlerData, &establisher, nullptr);
        }
    }
    if (PPCContext* c = t.ctx ? t.ctx : GuestContextOf(t.tid))
    {
        s.chain[s.depth++] = uint32_t(c->lr);
        uint32_t frame = c->r1.u32;
        for (int i = 0; i < 23 && frame && frame < 0xFFFFFFF0u; ++i)
        {
            const uint32_t prev = GRead32(g_base, frame);
            if (!prev || prev <= frame || prev - frame > 0x10000) break;
            s.chain[s.depth++] = GRead32(g_base, prev - 8);
            frame = prev;
        }
    }
    return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// A sample whose innermost host frame is a kernel wait / delay / yield stub (ntdll Nt* syscall
// stubs are short functions that end in syscall; NtWaitForSingleObject, NtDelayExecution,
// NtYieldExecution, NtSignalAndWaitForSingleObject, NtWaitForMultipleObjects, NtWaitForAlertByThreadId).
bool Waiting(uint64_t leaf)
{
    static const std::vector<uint64_t> stubs = [] {
        std::vector<uint64_t> v;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        for (const char* n : { "NtWaitForSingleObject", "NtDelayExecution", "NtYieldExecution", "NtSignalAndWaitForSingleObject",
                               "NtWaitForMultipleObjects", "NtWaitForAlertByThreadId", "NtRemoveIoCompletion", "ZwWaitForWorkViaWorkerFactory" })
            if (FARPROC f = GetProcAddress(ntdll, n)) v.push_back(reinterpret_cast<uint64_t>(f));
        return v;
    }();
    for (uint64_t a : stubs)
        if (leaf >= a && leaf < a + 0x20) return true;
    return false;
}

void ReportGaps()
{
    std::vector<Gap> gaps;
    {
        std::lock_guard l(g_gapsMutex);
        gaps.swap(g_gaps);
    }
    std::vector<Target*> targets;
    {
        std::lock_guard l(g_targetsMutex);
        targets = g_targets;
    }
    for (const Gap& gap : gaps)
    {
        if (g_gapReports++ >= 300) return;
        const int64_t start = gap.end - int64_t(gap.ms * Freq() / 1000.0);
        for (Target* t : targets)
        {
            std::vector<Sample> in;
            uint32_t busy = 0;
            for (const Sample& s : t->ring)
                if (s.at >= start && s.at <= gap.end)
                {
                    in.push_back(s);
                    busy += s.hostDepth && !Waiting(s.host[0]);
                }
            // Other threads only when they ran for a fifth of the gap (not parked in a wait).
            if (in.empty() || (t != g_main.load() && t->name != "cp" && busy * 5 < in.size())) continue;
            char header[96];
            std::snprintf(header, sizeof(header), "[gap] %.1f ms frame: %s thread (busy %u%%)", gap.ms, t->name.c_str(),
                          in.empty() ? 0u : unsigned(100.0 * busy / in.size() + 0.5));
            Report(header, in, 14, 16, 8);
        }
    }
}

void Loop()
{
    const int mode = Mode();
    const bool always = mode >= 2;
    const int64_t step = Freq() / (always ? 1000 : 2000);   // 1 / 0.5 ms
    int64_t windowStart = Now();
    for (;;)
    {
        if (!always && !g_active.load(std::memory_order_acquire)) { Sleep(1); continue; }
        std::vector<Target*> targets;
        {
            std::lock_guard l(g_targetsMutex);
            targets = g_targets;
        }
        for (Target* t : targets)
        {
            const bool main = t == g_main;
            if (!main && mode != 3 && !(mode == 4 && t->name == "cp")) continue;
            g_busy = main;
            const bool task = main && g_active.load(std::memory_order_acquire);
            Sample s;
            if ((task || always) && Take(*t, s))
            {
                if (mode == 3) { t->ring[t->next] = s; t->next = (t->next + 1) % t->ring.size(); }
                if ((mode == 2 && main) || mode == 4) t->window.push_back(s);
                std::lock_guard l(g_samplesMutex);
                if (task && g_samples.size() < 20000) g_samples.push_back(s);
            }
            g_busy = false;
        }
        const int64_t now = Now();
        if ((mode == 2 || mode == 4) && now - windowStart >= Freq() * 10)
        {
            for (Target* t : targets)
            {
                if (t->window.empty()) continue;
                const std::string header = "[profile] " + t->name + " thread, last 10 s";
                Report(header.c_str(), t->window, 40, 60, 30);
                t->window.clear();
            }
            windowStart = now;
        }
        if (mode == 3) ReportGaps();
        while (Now() < now + step) YieldProcessor();
    }
}

void Begin(PPCContext& ctx, uint8_t* base)
{
    if (!On()) return;
    (void)base;
    if (!g_main) Register("main", &ctx, true);
    {
        std::lock_guard l(g_samplesMutex);
        g_samples.clear();
    }
    g_active.store(true, std::memory_order_release);
}

void End(double ms, uint32_t vtable)
{
    if (!On()) return;
    g_active.store(false, std::memory_order_release);
    while (g_busy.load()) YieldProcessor();
    if (ms < 20.0) return;
    std::vector<Sample> samples;
    {
        std::lock_guard l(g_samplesMutex);
        samples.swap(g_samples);
    }
    char header[96];
    std::snprintf(header, sizeof(header), "[hitch] task vtable %08X %.1f ms", vtable, ms);
    Report(header, samples);
}

void Report(const char* header, const std::vector<Sample>& samples, size_t guestTop, size_t hostTop, size_t leafTop)
{
    std::fprintf(stderr, "%s, %zu samples\n", header, samples.size());
    if (samples.empty()) return;
    std::map<uint32_t, uint32_t> inclusive, innermost;
    std::map<uint64_t, uint32_t> hostInclusive, hostLeaf;
    for (const Sample& s : samples)
    {
        std::set<uint32_t> seen;
        for (uint32_t i = 0; i < s.depth; ++i)
        {
            const uint32_t f = FunctionOf(s.chain[i]);
            if (f && seen.insert(f).second) ++inclusive[f];
        }
        if (s.depth) ++innermost[FunctionOf(s.chain[0])];
        std::set<uint64_t> seenHost;
        for (uint32_t i = 0; i < s.hostDepth; ++i)
            if (seenHost.insert(s.host[i]).second) ++hostInclusive[s.host[i]];
        if (s.hostDepth) ++hostLeaf[s.host[0]];
    }
    auto top = [](const auto& m, size_t n) {
        std::vector<std::pair<uint32_t, typename std::decay_t<decltype(m)>::key_type>> v;
        for (const auto& [k, c] : m) v.push_back({ c, k });
        std::sort(v.rbegin(), v.rend());
        if (v.size() > n) v.resize(n);
        return v;
    };
    auto pct = [&](uint32_t c) { return std::to_string(unsigned(100.0 * c / samples.size() + 0.5)) + "%"; };
    for (const auto& [label, m, n] : { std::tuple{ "inclusive", &inclusive, guestTop }, std::tuple{ "innermost", &innermost, guestTop / 2 } })
    {
        if (m->empty()) continue;
        std::string out = std::string("[hitch]   ") + label + ":";
        for (const auto& [c, f] : top(*m, n))
        {
            char b[16];
            std::snprintf(b, sizeof(b), " %08X=", f);
            out += b + pct(c);
        }
        std::fprintf(stderr, "%s\n", out.c_str());
    }
    // exe functions as RVAs (rva2sym.py), others as module+RVA.
    auto where = [](uint64_t address) {
        HMODULE m = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(address), &m) && m)
            GetModuleFileNameA(m, path, sizeof(path));
        const char* name = std::strrchr(path, '\\');
        name = name ? name + 1 : path;
        char b[96];
        if (m == GetModuleHandleW(nullptr)) std::snprintf(b, sizeof(b), "%llX", (unsigned long long)(address - uint64_t(m)));
        else std::snprintf(b, sizeof(b), "%s+%llX", name, (unsigned long long)(address - uint64_t(m)));
        return std::string(b);
    };
    for (const auto& [label, m, n] : { std::tuple{ "host inclusive", &hostInclusive, hostTop }, std::tuple{ "host leaf", &hostLeaf, leafTop } })
    {
        std::string out = std::string("[hitch]   ") + label + ":";
        for (const auto& [c, a] : top(*m, n)) out += " " + where(a) + "=" + pct(c);
        std::fprintf(stderr, "%s\n", out.c_str());
    }
}
} // namespace hitch

// Mode 3 hooks for kernel_video.cpp: the command-processor thread registers itself, and the
// present timer reports frames over 25 ms.
// data_watch.cpp: the guest context a host thread is running (its LR names the guest caller).
PPCContext* WatchGuestContextOf(DWORD tid) { return hitch::GuestContextOf(tid); }

void HitchRegisterThread(const char* name)
{
    if (hitch::Mode() == 3 || (hitch::Mode() == 4 && std::strcmp(name, "cp") == 0)) hitch::Register(name, nullptr);
}
void HitchFrameGap(double ms)
{
    if (hitch::Mode() != 3) return;
    std::lock_guard l(hitch::g_gapsMutex);
    hitch::g_gaps.push_back({ hitch::Now(), ms });
}

// TTT2_HITCH_PROFILE: the guest heap's RtlAllocateHeap (82BFA928: r3 heap, r4 flags, r5 size)
// and RtlFreeHeap (82BFB220) counted and timed per thread, reported with each slow task: many
// cheap calls (allocation-heavy code) or a few slow ones (long free-list walks)?
namespace heapstat {
struct Counts { uint64_t allocs = 0, frees = 0; double allocMs = 0, freeMs = 0, slowest = 0; uint32_t slowestSize = 0; };
thread_local Counts t_counts;
inline double Ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
    static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return 1000.0 / double(q.QuadPart); }();
    return double(b.QuadPart - a.QuadPart) * f;
}
}
extern "C" PPC_FUNC(__imp__sub_82BFA928);
PPC_FUNC(sub_82BFA928)
{
    if (!hitch::On()) { __imp__sub_82BFA928(ctx, base); return; }
    const uint32_t size = ctx.r5.u32;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    __imp__sub_82BFA928(ctx, base);
    QueryPerformanceCounter(&b);
    auto& c = heapstat::t_counts;
    const double ms = heapstat::Ms(a, b);
    ++c.allocs;
    c.allocMs += ms;
    if (ms > c.slowest) { c.slowest = ms; c.slowestSize = size; }
}
extern "C" PPC_FUNC(__imp__sub_82BFB220);
PPC_FUNC(sub_82BFB220)
{
    if (!hitch::On()) { __imp__sub_82BFB220(ctx, base); return; }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    __imp__sub_82BFB220(ctx, base);
    QueryPerformanceCounter(&b);
    auto& c = heapstat::t_counts;
    const double ms = heapstat::Ms(a, b);
    ++c.frees;
    c.freeMs += ms;
    if (ms > c.slowest) { c.slowest = ms; c.slowestSize = 0; }
}

void ThreadIoTotals(uint64_t& calls, uint64_t& bytes, double& ms);   // kernel_io.cpp
void CtabCacheCounts(uint64_t& calls, uint64_t& hits);              // d3dx_ctab_cache.cpp
// The game's task runner (task->vtable[1] = step, called until it stops): name the task.
extern "C" PPC_FUNC(__imp__sub_82A80AD8);
PPC_FUNC(sub_82A80AD8)
{
    static const bool on = [] { const char* e = std::getenv("TTT2_FRAMETIME"); return e && *e == '1'; }();
    if (!on && !hitch::On()) { __imp__sub_82A80AD8(ctx, base); return; }       // no diagnostics: nothing added
    const uint32_t task = ctx.r3.u32, vtable = task ? GRead32(base, task) : 0, step = vtable ? GRead32(base, vtable + 4) : 0;
    const uint32_t lr = uint32_t(ctx.lr);
    uint64_t calls0 = 0, bytes0 = 0, calls1 = 0, bytes1 = 0;
    double io0 = 0, io1 = 0;
    ThreadIoTotals(calls0, bytes0, io0);
    static const DWORD mainThread = GetCurrentThreadId();
    thread_local int depth = 0;
    const bool outer = GetCurrentThreadId() == mainThread && depth++ == 0;
    if (outer) hitch::Begin(ctx, base);
    const heapstat::Counts heap0 = heapstat::t_counts;
    uint64_t ctabCalls0 = 0, ctabHits0 = 0;
    CtabCacheCounts(ctabCalls0, ctabHits0);
    const auto start = std::chrono::steady_clock::now();
    __imp__sub_82A80AD8(ctx, base);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (GetCurrentThreadId() == mainThread) --depth;
    if (outer) hitch::End(ms, vtable);
    if (outer && ms >= 20.0 && hitch::On())
    {
        const heapstat::Counts& h = heapstat::t_counts;
        std::fprintf(stderr, "[hitch]   heap: %llu allocs %.1f ms, %llu frees %.1f ms (thread's slowest call so far %.2f ms, size %u)\n",
                     (unsigned long long)(h.allocs - heap0.allocs), h.allocMs - heap0.allocMs,
                     (unsigned long long)(h.frees - heap0.frees), h.freeMs - heap0.freeMs, h.slowest, h.slowestSize);
        uint64_t ctabCalls = 0, ctabHits = 0;
        CtabCacheCounts(ctabCalls, ctabHits);
        std::fprintf(stderr, "[hitch]   constant tables: %llu requests, %llu from the cache\n",
                     (unsigned long long)(ctabCalls - ctabCalls0), (unsigned long long)(ctabHits - ctabHits0));
    }
    if (!on) return;
    ThreadIoTotals(calls1, bytes1, io1);
    static std::atomic<int> logged{ 0 };
    if (ms > 8.0 && logged++ < 400)
        std::fprintf(stderr, "[frame]   file calls during it: %llu, %llu KB, %.1f ms\n", (unsigned long long)(calls1 - calls0),
                     (unsigned long long)((bytes1 - bytes0) / 1024), io1 - io0);
    if (ms > 8.0 && logged < 400)
        std::fprintf(stderr, "[frame] slow task %08X vtable %08X step %08X %.1f ms (lr %08X) thread %lu\n", task, vtable, step, ms, lr,
                     GetCurrentThreadId());
}
TIMED_HOOK(82258D60)
TIMED_HOOK(825DEBD0)
TIMED_HOOK(822D36C8)
