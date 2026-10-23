// In-process TCP endpoints on the virtual network (xsocket.cpp): a "service peer" answers streams the game opens
// to its address - Namco's GIN web service for the community servers (port/COMMUNITY_SERVER_PLAN.md 4.5). A
// connect to a service peer succeeds at once; the bytes the game sends go to the service's handler, which answers
// later from any thread. Service peer ids live in their own namespace (kServicePeer | service id), apart from
// SteamIDs and the loopback transport's ids.
#pragma once
#include <cstddef>
#include <cstdint>

namespace xsvc {

constexpr uint64_t kServicePeer = 0x5356430000000000ull;      // "SVC" | the Xbox LIVE service id

// Called with the socket layer's lock held: queue the work, never block or call back into xsvc from here.
struct Handler
{
    virtual ~Handler() = default;
    virtual void Open(uint32_t conn, uint16_t port) = 0;
    virtual void Data(uint32_t conn, const uint8_t* data, size_t size) = 0;
    virtual void Close(uint32_t conn) = 0;                    // the game closed or shut down its side
};

void Register(uint64_t peer, Handler* handler);
Handler* Find(uint64_t peer);
void Reply(uint64_t peer, uint32_t conn, const uint8_t* data, size_t size);   // to the game; any thread
void Finish(uint64_t peer, uint32_t conn);                                     // the service closed its side

} // namespace xsvc
