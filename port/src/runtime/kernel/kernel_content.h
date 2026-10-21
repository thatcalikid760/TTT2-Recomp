#pragma once
#include <cstdint>

// Bounded XAM completion support: polling and event handles are supported.
// Initiating-thread APC callbacks are rejected before starting an operation.
uint32_t XamValidateOverlapped(uint8_t* base, uint32_t overlapped);
uint32_t XamCompleteOverlapped(uint8_t* base, uint32_t overlapped,
                             uint32_t result, uint32_t length, uint32_t extendedError);
bool XamGuestRange(uint32_t address, uint64_t size, uint32_t alignment = 4);

// Returns true for the reserved content handle range, including stale handles.
bool TryCloseContentEnumerator(uint32_t handle, uint32_t& status);
