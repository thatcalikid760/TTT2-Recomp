#pragma once
#include <cstdint>

// PM4 headers count payload DWORDs minus one. Type 1 always has two payload
// DWORDs; type 2 and zero padding have none. TSWP is our five-DWORD swap marker.
inline uint32_t GpuPacketBytes(uint32_t header)
{
    if (!header) return 4;
    if (header == 0x54535750u) return 20;
    const uint32_t type = header >> 30;
    if (type == 1) return 12;
    if (type == 2) return 4;
    return ((((header >> 16) & 0x3FFF) + 1) + 1) * 4;
}

inline constexpr uint32_t GpuMaxPacketBytes = (0x4000 + 1) * 4;

struct GpuRingPacket
{
    uint32_t offset, size, firstBytes, header;
    bool Wraps() const { return firstBytes != size; }
};

enum class GpuRingStatus { Complete, InvalidRange, IncompletePacket };
struct GpuRingResult
{
    GpuRingStatus status;
    uint32_t offset, remaining, required, header;
};

// The producer wraps EACH word of a packet at the ring boundary. Execute only
// whole packets inside the published interval, never two independent fragments.
// Read takes a ring-relative byte offset; execute receives both fragment sizes.
template<class Read, class Execute>
GpuRingResult WalkGpuRing(uint32_t size, uint32_t read, uint32_t write,
                         Read&& readWord, Execute&& execute)
{
    if (size < 4 || (size & (size - 1)) || read >= size || write >= size ||
        ((read | write) & 3))
        return {GpuRingStatus::InvalidRange, read, 0, 0, 0};
    uint32_t remaining = (write - read) & (size - 1);
    while (remaining)
    {
        const uint32_t header = readWord(read);
        const uint32_t bytes = GpuPacketBytes(header);
        if (bytes > remaining)
            return {GpuRingStatus::IncompletePacket, read, remaining, bytes, header};
        const uint32_t toEnd = size - read;
        execute(GpuRingPacket{read, bytes, bytes < toEnd ? bytes : toEnd, header});
        read = (read + bytes) & (size - 1);
        remaining -= bytes;
    }
    return {GpuRingStatus::Complete, read, 0, 0, 0};
}
