#include "guest_heap.h"
#include <atomic>
#include <cstdio>
#include <cstring>

GuestHeap g_virtualHeap;
GuestHeap g_physicalHeap;

// Implicit free list with boundary tags. Each block: [Block header][payload...].
// size = total block size including header, always 16-aligned. free flag in low bit of size.
struct GuestHeap::Block
{
    size_t sizeAndFree;   // total bytes incl header; bit0 = free
    Block* next;          // physical next (contiguous)
    Block* prev;          // physical prev
    // payload follows
    size_t Size() const { return sizeAndFree & ~size_t(1); }
    bool   Free() const { return sizeAndFree & 1; }
    void   Set(size_t sz, bool f) { sizeAndFree = sz | (f ? 1 : 0); }
    uint8_t* payload() { return reinterpret_cast<uint8_t*>(this) + sizeof(Block); }
};

static constexpr size_t HDR = sizeof(GuestHeap::Block);
static constexpr size_t MIN_BLOCK = HDR + 16;

void GuestHeap::Init(uint8_t* hostBase, uint32_t guestBegin, uint32_t guestEnd)
{
    m_lo = hostBase + guestBegin;
    m_hi = hostBase + guestEnd;
    m_head = reinterpret_cast<Block*>(m_lo);
    m_head->Set(size_t(m_hi - m_lo), true);
    m_head->next = nullptr;
    m_head->prev = nullptr;
}

GuestHeap::Block* GuestHeap::firstFit(size_t need)
{
    for (Block* b = m_head; b; b = b->next)
        if (b->Free() && b->Size() >= need)
            return b;
    return nullptr;
}

std::atomic<uint64_t> g_guestHeapOps{ 0 };   // allocations + frees (rollback.cpp measures them per frame)

void* GuestHeap::Alloc(size_t size, size_t alignment)
{
    g_guestHeapOps.fetch_add(1, std::memory_order_relaxed);
    if (alignment < 16) alignment = 16;
    std::lock_guard lock(m_mutex);

    // Over-allocate so we can align the payload and still store a back-pointer to the block.
    size_t need = HDR + size + alignment + 16;
    need = (need + 15) & ~size_t(15);

    Block* b = firstFit(need);
    if (!b) return nullptr;

    // Split off the remainder if the block is much larger.
    if (b->Size() >= need + MIN_BLOCK)
    {
        Block* rem = reinterpret_cast<Block*>(reinterpret_cast<uint8_t*>(b) + need);
        rem->Set(b->Size() - need, true);
        rem->prev = b;
        rem->next = b->next;
        if (rem->next) rem->next->prev = rem;
        b->next = rem;
        b->Set(need, false);
    }
    else
    {
        b->Set(b->Size(), false);
    }

    // Align payload; stash the owning Block* in the word just before the returned pointer.
    uintptr_t raw = reinterpret_cast<uintptr_t>(b->payload());
    uintptr_t aligned = (raw + sizeof(void*) + (alignment - 1)) & ~uintptr_t(alignment - 1);
    reinterpret_cast<Block**>(aligned)[-1] = b;
    return reinterpret_cast<void*>(aligned);
}

void GuestHeap::coalesce(Block* b)
{
    if (b->next && b->next->Free())
    {
        Block* n = b->next;
        b->Set(b->Size() + n->Size(), true);
        b->next = n->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->Free())
    {
        Block* p = b->prev;
        p->Set(p->Size() + b->Size(), true);
        p->next = b->next;
        if (p->next) p->next->prev = p;
    }
}

// Alloc stashes the owning Block* in the word immediately before the pointer it returns, so
// Free/Size can find it again. The game does NOT only hand back pointers this heap produced -
// it reaches free() on pointers from its own allocators and on payloads whose preceding word it
// has overwritten - and dereferencing one of those killed a whole run (a null Block* inside
// GuestHeap::Free). Validate the back-pointer instead: it has to land inside this heap, be
// block-aligned, and describe a block that fits. Anything else is reported once and ignored,
// which leaks at worst where the old code crashed.
GuestHeap::Block* GuestHeap::resolve(void* ptr)
{
    if (!ptr) return nullptr;
    auto* slot = reinterpret_cast<uint8_t*>(ptr) - sizeof(Block*);
    if (slot < m_lo || slot + sizeof(Block*) > m_hi) return nullptr;
    Block* b = *reinterpret_cast<Block**>(slot);
    auto* p = reinterpret_cast<uint8_t*>(b);
    if (!b || p < m_lo || p + HDR > m_hi || ((p - m_lo) & 15) != 0) return nullptr;
    size_t sz = b->Size();
    if (sz < MIN_BLOCK || sz > size_t(m_hi - p)) return nullptr;
    return b;
}

static void ReportForeign(const char* what, void* ptr)
{
    static int budget = 8;
    if (budget-- <= 0) return;
    std::fprintf(stderr, "[heap] %s on a pointer this heap did not allocate (%p), ignored\n",
                 what, ptr);
}

void GuestHeap::Free(void* ptr)
{
    if (!ptr) return;
    g_guestHeapOps.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(m_mutex);
    Block* b = resolve(ptr);
    if (!b) { ReportForeign("free", ptr); return; }
    b->Set(b->Size(), true);
    coalesce(b);
}

size_t GuestHeap::Size(void* ptr)
{
    if (!ptr) return 0;
    std::lock_guard lock(m_mutex);
    Block* b = resolve(ptr);
    if (!b) { ReportForeign("size", ptr); return 0; }
    return b->Size() - HDR;
}
