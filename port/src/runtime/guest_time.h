#pragma once
#include <cstdint>

// Xenon's time-base counter runs at 50 MHz. Keep MFTB and the kernel query
// consistent, independent of the host CPU frequency and power state.
inline constexpr uint64_t PPC_TIME_BASE_FREQUENCY = 50000000;
inline constexpr uint64_t GuestTimebaseFromNanoseconds(uint64_t nanoseconds)
{
    return nanoseconds / 20;
}

uint64_t PPCReadTimebase();
