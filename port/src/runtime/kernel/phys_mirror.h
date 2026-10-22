#pragma once
// Cached physical view (guest 0xC0000000-0xDFFFFFFF) kept coherent with the 0xA0000000 view
// the port allocates physical memory in. See phys_mirror.cpp.
#include <cstdint>

// Close the whole C view (PAGE_NOACCESS) and install the fault handler that fills it.
void PhysMirrorInit(uint8_t* base);
// Bytes of the A view in [start, start + size) changed behind the CPU (a GPU resolve):
// close the C-view chunks that copied them, so the next read refills.
void PhysMirrorInvalidate(uint32_t start, uint32_t size);
// Close every open chunk (frame boundary): bounds staleness after CPU writes to the A view.
void PhysMirrorInvalidateAll();

// A-view guards. TTT2 also reads resolves straight through the A view (the customization screen's
// card for a new custom character: its thumbnail is compressed on the CPU from a resolve, and came
// out all zero - a black card). A GPU-only resolve's pages are PAGE_GUARD; the CPU's first touch of
// one writes the resolve back before the access completes. TTT2_RESOLVE_GUARD=0 turns it off.
void PhysGuardResolve(uint32_t start, uint32_t size);   // GPU backend: after a GPU-only resolve
void PhysUnguard(uint32_t start, uint32_t size);        // GPU backend: once these bytes are coherent
// Host code about to hand guest memory to Windows (ReadFile, ReadProcessMemory), which fails on a
// guard page instead of faulting: write back any resolve there and drop the guards.
void PhysPrepareHostAccess(uint32_t start, uint32_t size);
