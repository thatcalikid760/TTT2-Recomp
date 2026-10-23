// Re-simulated frames' skinned vertices in scratch memory (rollback sessions, rollback.cpp).
//
// TTT2's skinning jobs write each frame's skinned vertices into a ring of 3 regions (the object at
// 82E40580: +4 region size, +8.. region bases, +20 index, +24 cursor, +68 region count; sub_82AD5BA0
// advances it once a frame, sub_82AD5B40 bumps the cursor). A frame still being drawn by the command
// processor reads its region; a rewind restores the ring's index and the re-simulated frames then
// write the same regions again - MEASURED 2026-10-20 (TTT2_RB_GPUMEM): of everything the frame in
// flight reads, only its skinned vertices change during a rewind (37-77 pages after the first
// re-simulated update alone, 60-130 by the end; command buffers, index data, constants: 0). So a
// rewind used to wait for the GPU to finish that frame first (~4.4 ms here).
//
// Re-simulated frames are never shown (GpuDiscardNextFrames): their skinned vertices can live
// anywhere. While a re-simulation runs - and until the first live advance after it, which catches the
// live frame's own update and skinning jobs the last re-simulated draw left running - the ring's
// allocations come from scratch memory instead: one ring of 4 region sizes that only moves forward
// (wrapping at its end), never rewound. The first live frame after a rewind reads the last
// re-simulated frame's vertices there (as the previous frame's, MEASURED: both the part its update
// placed and its draw's), so data must outlive a couple of frames: per-frame slots that reset broke
// that (gm72: 7.3 pages a rewind changed under the frame in flight); a forward-only ring keeps ~10
// re-simulated frames (~3 MB each). Each frame still gets at most a region's size, as from the real
// ring, so the game places (or skips) exactly what it would. The ring's own index still advances as
// before, so the live frames that follow use the regions they always would; the offset handed back
// stays "address minus the current region's base", as the game computes it.
// TTT2_RB_SCRATCH=0 turns this off (the rewind then waits for the GPU again).
#include "kernel_shared.h"
#include "guest_heap.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_82AD5B40);

namespace {
std::atomic<bool> g_on{ false };              // allocations go to scratch
std::atomic<bool> g_resim{ false };           // a re-simulation is running (rollback.cpp)
std::atomic<uint32_t> g_cursor{ 0 };          // next free byte in the scratch ring
std::atomic<uint32_t> g_frameUsed{ 0 };       // bytes placed since the last advance (a region's worth at most)
uint32_t g_scratch = 0, g_ringBytes = 0, g_regionSize = 0;
std::atomic<uint64_t> g_allocs{ 0 }, g_bytes{ 0 }, g_refused{ 0 };
std::atomic<uint32_t> g_rewindNo{ 0 };        // re-simulations begun (diagnostics: page ages)
std::vector<uint32_t> g_pageTag, g_prevTag;   // scratch page -> g_rewindNo + 1 when last allocated over / the tag before

bool Enabled()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_RB_SCRATCH"); return !(e && *e == '0'); }();
    return on;
}
} // namespace

// rollback.cpp: the never-rewound ranges get the scratch (allocated once, at the first fight's
// calibration, sized from the skinned ring's region size). False: no scratch (rewinds drain).
bool ResimScratchReady(uint8_t* base, std::vector<std::pair<uint32_t, uint32_t>>& neverRewind)
{
    if (!Enabled()) return false;
    if (g_scratch) return true;
    constexpr uint32_t kRing = 0x82E40580;
    const uint32_t regionSize = GRead32(base, kRing + 4), regions = GRead32(base, kRing + 68);
    if (regions < 2 || regions > 8 || regionSize < 0x10000 || regionSize > (64u << 20))
    {
        std::fprintf(stderr, "[rb-scratch] skinned ring not set up (regions %u, size %08X): rewinds wait for the GPU\n", regions, regionSize);
        return false;
    }
    for (uint32_t n : { 4u, 3u })
        if (void* p = g_physicalHeap.Alloc(size_t(regionSize) * n, 0x1000))
        {
            g_scratch = static_cast<uint32_t>(static_cast<uint8_t*>(p) - base);
            g_ringBytes = regionSize * n;
            g_regionSize = regionSize;
            g_pageTag.assign(g_ringBytes >> 12, 0);
            g_prevTag.assign(g_ringBytes >> 12, 0);
            neverRewind.push_back({ g_scratch, g_ringBytes });
            std::fprintf(stderr, "[rb-scratch] re-simulated frames' skinned vertices go to %08X (%u MB, %u regions' worth)\n",
                         g_scratch, g_ringBytes >> 20, n);
            return true;
        }
    std::fprintf(stderr, "[rb-scratch] no %u MB of physical memory for the scratch: rewinds wait for the GPU\n", regionSize * 3 >> 20);
    return false;
}
bool ResimScratchOn() { return g_scratch != 0; }

// rollback.cpp, around a session rewind's re-simulation (after the restore). The first re-simulated
// update allocates before any advance: the frame's usage so far is the restored ring's, as it was.
void ResimScratchBegin()
{
    if (!g_scratch) return;
    constexpr uint32_t kRing = 0x82E40580;
    const uint32_t index = GRead32(g_base, kRing + 20), cursor = GRead32(g_base, kRing + 24);
    const uint32_t regionBase = index < 8 ? GRead32(g_base, kRing + 8 + 4 * index) : cursor;
    g_frameUsed.store(cursor >= regionBase && cursor - regionBase <= g_regionSize ? cursor - regionBase : 0);
    g_rewindNo.fetch_add(1);
    g_resim.store(true);
    g_on.store(true);
}
// Diagnostics: -1 outside the scratch, else rewinds since the page was last allocated over.
int ResimScratchPageAge(uint32_t page)
{
    if (!g_scratch || page < (g_scratch >> 12) || page >= ((g_scratch + g_ringBytes) >> 12)) return -1;
    const uint32_t tag = g_prevTag[page - (g_scratch >> 12)];
    return tag ? int(g_rewindNo.load() - (tag - 1)) : 1 << 20;
}
void ResimScratchEnd() { g_resim.store(false); }   // g_on stays until the next live advance
void ResimScratchStats(uint64_t& allocs, uint64_t& bytes, uint64_t& refused)
{
    allocs = g_allocs.load(); bytes = g_bytes.load(); refused = g_refused.load();
}

// The ring's cursor bump: r3 ring, r4 bytes, r5 where the offset from the region base goes (or 0);
// returns the old cursor (0 when the region is full).
PPC_FUNC(sub_82AD5B40)
{
    if (!g_on.load(std::memory_order_acquire)) { __imp__sub_82AD5B40(ctx, base); return; }
    const uint32_t ring = ctx.r3.u32, size = ctx.r4.u32, out = ctx.r5.u32;
    const uint32_t index = GRead32(base, ring + 20);
    const uint32_t regionBase = GRead32(base, ring + 8 + 4 * index);
    // A region's worth a frame, as the real ring gives.
    if (g_frameUsed.fetch_add(size, std::memory_order_relaxed) + uint64_t(size) > g_regionSize || size > g_ringBytes)
    {
        g_frameUsed.fetch_sub(size, std::memory_order_relaxed);   // as the real ring: a refusal takes nothing
        g_refused.fetch_add(1, std::memory_order_relaxed);
        if (out) GWrite32(base, out, 0);
        ctx.r3.u64 = 0;
        return;
    }
    uint32_t at = g_cursor.load(std::memory_order_relaxed);
    uint32_t start;
    for (;;)
    {
        start = uint64_t(at) + size > g_ringBytes ? 0u : at;   // wrap: never split one allocation
        if (g_cursor.compare_exchange_weak(at, start + size, std::memory_order_relaxed)) break;
    }
    const uint32_t addr = g_scratch + start;
    const uint32_t now = g_rewindNo.load(std::memory_order_relaxed) + 1;
    for (uint32_t pg = start >> 12; pg <= (start + size - 1) >> 12 && pg < g_pageTag.size(); ++pg)
        if (g_pageTag[pg] != now) { g_prevTag[pg] = g_pageTag[pg]; g_pageTag[pg] = now; }
    if (out) GWrite32(base, out, addr - regionBase);
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(size, std::memory_order_relaxed);
    ctx.r3.u64 = addr;
}

// diag_hooks.cpp's sub_82AD5BA0 (the ring advance) calls this first: a new frame's worth while
// re-simulating; the first live advance after a re-simulation ends the scratch.
void ResimScratchAdvance()
{
    if (!g_on.load(std::memory_order_acquire)) return;
    if (!g_resim.load(std::memory_order_acquire)) { g_on.store(false, std::memory_order_release); return; }
    g_frameUsed.store(0, std::memory_order_relaxed);
}
