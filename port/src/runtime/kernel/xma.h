#pragma once
// XMA audio decoder hardware emulation (xma.cpp; decoder core in xma_core.h).
#include <cstdint>

// Allocate the 320-context array in physical memory and publish its address in the XMA block's
// context-array register. Call once after the guest heaps exist, before the title starts.
void XmaInit(uint8_t* base);
