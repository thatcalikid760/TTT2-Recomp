#include "guest_time.h"
#include <chrono>

uint64_t PPCReadTimebase()
{
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    return GuestTimebaseFromNanoseconds(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
}
