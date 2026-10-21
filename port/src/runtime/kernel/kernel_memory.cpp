// Memory-management kernel imports (real implementations).
#include "kernel_shared.h"
#include "guest_heap.h"
#include "kernel_trace.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// NtAllocateVirtualMemory(r3=PVOID* BaseAddress, r4=PSIZE_T RegionSize, r5=AllocType, r6=Protect)
// The whole guest space is committed, so honor a requested address as-is; otherwise carve from the heap.
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    KTrace("NtAllocateVirtualMemory");
    uint32_t basePtr = ctx.r3.u32;
    uint32_t sizePtr = ctx.r4.u32;
    uint32_t reqBase = basePtr ? GRead32(base, basePtr) : 0;
    uint32_t size    = sizePtr ? GRead32(base, sizePtr) : 0;
    uint32_t rounded = (size + 0xFFFF) & ~0xFFFFu;   // 64 KiB granularity
    if (rounded == 0) rounded = 0x10000;

    uint32_t addr = reqBase;
    if (addr == 0)
    {
        void* p = g_virtualHeap.Alloc(rounded, 0x10000);
        if (!p) { ctx.r3.u32 = STATUS_UNSUCCESSFUL; return; }
        addr = GuestAddr(base, p);
    }
    GWrite32(base, basePtr, addr);
    GWrite32(base, sizePtr, rounded);
    ctx.r3.u32 = STATUS_SUCCESS;
}

// NtFreeVirtualMemory(r3=PVOID* BaseAddress, r4=PSIZE_T RegionSize, r5=FreeType) -> honor best-effort.
PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    KTrace("NtFreeVirtualMemory");
    uint32_t basePtr = ctx.r3.u32;
    uint32_t addr = basePtr ? GRead32(base, basePtr) : 0;
    if (addr) { void* h = base + addr; if (g_virtualHeap.Owns(h)) g_virtualHeap.Free(h); }
    ctx.r3.u32 = STATUS_SUCCESS;
}

// ExAllocatePool(r3=size) / ExAllocatePoolTypeWithTag(r3=size, r4=tag, r5=type) -> guest pool ptr in r3.
PPC_FUNC(__imp__ExAllocatePool)
{
    KTrace("ExAllocatePool");
    void* p = g_virtualHeap.Alloc(ctx.r3.u32 ? ctx.r3.u32 : 16, 16);
    ctx.r3.u32 = p ? GuestAddr(base, p) : 0;
}
PPC_FUNC(__imp__ExAllocatePoolTypeWithTag)
{
    KTrace("ExAllocatePoolTypeWithTag");
    void* p = g_virtualHeap.Alloc(ctx.r3.u32 ? ctx.r3.u32 : 16, 16);
    if (p) std::memset(p, 0, ctx.r3.u32);
    ctx.r3.u32 = p ? GuestAddr(base, p) : 0;
}
PPC_FUNC(__imp__ExFreePool)
{
    KTrace("ExFreePool");
    if (ctx.r3.u32) { void* h = base + ctx.r3.u32; if (g_virtualHeap.Owns(h)) g_virtualHeap.Free(h); }
}

// MmAllocatePhysicalMemoryEx(r3=flags, r4=size, r5=protect, r6=min, r7=max, r8=alignment) -> phys guest addr.
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    KTrace("MmAllocatePhysicalMemoryEx");
    uint32_t size = ctx.r4.u32;
    uint32_t align = ctx.r8.u32 ? ctx.r8.u32 : 0x1000;
    const uint32_t flags = ctx.r3.u32;
    void* p = g_physicalHeap.Alloc(size ? size : 0x1000, align);
    ctx.r3.u32 = p ? GuestAddr(base, p) : 0;
    static const bool diag = [] { const char* e = std::getenv("TTT2_DIAG_BAKE"); return e && *e == '1'; }();
    if (diag && size >= 0x40000)
        std::fprintf(stderr, "[diag-bake] phys alloc flags=%08X size=%08X protect=%08X align=%08X -> %08X\n",
            flags, size, ctx.r5.u32, align, ctx.r3.u32);
}
PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    KTrace("MmFreePhysicalMemory");
    // r3 = type, r4 = address (Xbox signature); free the physical allocation.
    uint32_t addr = ctx.r4.u32;
    if (addr) { void* h = base + addr; if (g_physicalHeap.Owns(h)) g_physicalHeap.Free(h); }
}

// GPU sees the same flat memory: physical address == virtual offset.
PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    KTrace("MmGetPhysicalAddress"); /* r3 stays: identity mapping */ }
// MmMapIoSpace(r3=?, r4=physical address, r5=size, r6=protect) -> virtual address. TTT2's XAudio2
// maps XMA contexts with it; physical and virtual addresses are the same in the port.
PPC_FUNC(__imp__MmMapIoSpace)
{
    KTrace("MmMapIoSpace"); ctx.r3.u32 = ctx.r4.u32; }
PPC_FUNC(__imp__MmQueryAddressProtect)
{
    KTrace("MmQueryAddressProtect"); ctx.r3.u32 = 4; /* PAGE_READWRITE */ }
PPC_FUNC(__imp__MmQueryAllocationSize)
{
    KTrace("MmQueryAllocationSize");
    void* h = base + ctx.r3.u32;
    size_t sz = g_virtualHeap.Owns(h) ? g_virtualHeap.Size(h) : (g_physicalHeap.Owns(h) ? g_physicalHeap.Size(h) : 0);
    ctx.r3.u32 = static_cast<uint32_t>(sz);
}
