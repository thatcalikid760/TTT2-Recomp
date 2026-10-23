// Packet transport between game instances (port/ONLINE_PLAN.md): the virtual sockets
// (xsocket.cpp) and the session layer send whole packets to a peer id; the backend delivers them.
//  - steam (default when the Steam client runs; steam_net.cpp): SteamNetworkingMessages over
//    Valve's relays, peer id = SteamID.
//  - loopback: UDP on 127.0.0.1, one port per instance (28100 + n). Peer id =
//    0x4C4F4F50'0000xxxx with the port in the low bits. For two instances on one PC (both would
//    have the same SteamID). TTT2_NET=loopback | steam forces a backend.
#pragma once
#include <cstddef>
#include <cstdint>

namespace net {

// Channels: reliable ordered (stream sockets, session control) or unreliable (datagrams).
enum class Channel : uint8_t { Unreliable = 0, Reliable = 1 };

using Receiver = void (*)(uint64_t fromPeer, Channel channel, const uint8_t* data, size_t size);

// Starts the backend once (idempotent); packets arrive on its own thread through `receiver`.
bool Start(Receiver receiver);
uint64_t LocalPeer();                   // 0 before Start
bool Send(uint64_t peer, Channel channel, const void* data, size_t size);
const char* BackendName();

} // namespace net
