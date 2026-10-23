#pragma once
// Guest heap: a first-fit free-list allocator over a region of the guest address space.
// Returns host pointers inside the committed guest block; callers map to guest addresses
// via (ptr - g_base). Two heaps: general (virtual) and physical (GPU) memory.
#include <cstdint>
#include <cstddef>
#include <mutex>

class GuestHeap
{
public:
    void Init(uint8_t* hostBase, uint32_t guestBegin, uint32_t guestEnd);
    void* Alloc(size_t size, size_t alignment = 16);
    void  Free(void* ptr);
    size_t Size(void* ptr);          // usable size of an allocation
    bool  Owns(const void* ptr) const { return ptr >= m_lo && ptr < m_hi; }

    struct Block;                    // boundary-tagged block header (impl detail, exposed for sizeof)

private:
    Block* firstFit(size_t need);
    void   coalesce(Block* b);
    Block* resolve(void* ptr);       // validated payload -> owning block, or null

    std::mutex m_mutex;
    uint8_t* m_lo = nullptr;
    uint8_t* m_hi = nullptr;
    Block*  m_head = nullptr;
};

extern GuestHeap g_virtualHeap;      // NtAllocateVirtualMemory / RtlAllocateHeap / ExAllocatePool
extern GuestHeap g_physicalHeap;     // MmAllocatePhysicalMemory (GPU-visible)
