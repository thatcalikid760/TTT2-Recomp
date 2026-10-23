// Packet transport between game instances (port/ONLINE_PLAN.md): the virtual sockets
// (xsocket.cpp) and the session layer send whole packets to a peer id; the backend delivers them.
//  - steam (default when the Steam client runs; steam_net.cpp): SteamNetworkingMessages over
//    Valve's relays, peer id = SteamID.
//  - loopback: UDP on 127.0.0.1, one port per instance (28100 + n). Peer id =
//    0x4C4F4F50'0000xxxx with the port in the low bits. For two instances on one PC (both would
//    have the same SteamID). TTT2_NET=loopback | steam forces a backend.
//  - a community ttt2-server (community_net.cpp, port/COMMUNITY_SERVER_PLAN.md), chosen in the game
//    (ONLINE MODE -> SERVER, server_ui.cpp), is where players find each other: sessions, invites and the
//    game's GIN requests go to it (ConnectServer; BackendName() "server"), while the matches themselves keep
//    running over Steam P2P - the server checks each player's Steam ID with Steam and lists sessions under
//    it. TTT2_SERVER_TEST_ID (tests): with the loopback transport, its peer id is the identity on a server
//    that trusts unverified IDs (two instances on one PC). Server keys are pinned in
//    %LOCALAPPDATA%\TTT2Recomp\community\known_servers.txt (TTT2_KNOWN_SERVERS = another file, for tests).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace community { class Client; struct Welcome; }

namespace net {

// Channels: reliable ordered (stream sockets, session control) or unreliable (datagrams).
enum class Channel : uint8_t { Unreliable = 0, Reliable = 1 };

using Receiver = void (*)(uint64_t fromPeer, Channel channel, const uint8_t* data, size_t size);

// Starts the backend once (idempotent); packets arrive on its own thread through `receiver`.
bool Start(Receiver receiver);
uint64_t LocalPeer();                   // 0 before Start
bool Send(uint64_t peer, Channel channel, const void* data, size_t size);
const char* BackendName();

// The game's online protocol: only games of the same one meet (Steam lobbies, community servers).
const char* GameProtocol();
// The community server's client while one is the network, else nullptr.
std::shared_ptr<community::Client> Community();

// Logs in to a community server (blocking, about 8 s at most: not on a game thread) and makes it where players
// find each other, replacing an earlier one (matches keep the transport above). False with what the player
// should read in `error`, and the server's refusal (community::Refusal) in `refusal` when it said no.
// banUntil (when not null): after a RefusedBanned, the ban's end the server told this player (unix seconds, 0 = for
// good), ~0 when it did not say.
bool ConnectServer(const std::string& address, const std::string& password, community::Welcome& welcome,
                   std::string& error, int& refusal,
                   uint64_t* banUntil = nullptr);
// Logs in to the community server at `address` and leaves at once, leaving the network as it is (blocking, not on a
// game thread): does it still turn this player away? 1 let in, 0 banned (or kicked: `banUntil` until when, ~0 not
// said), -1 no answer or another refusal. The server list asks it about the servers that banned this player (a ban
// lifted early).
int ProbeServer(const std::string& address, const std::string& password, uint64_t& banUntil);
// Back to Steam (or loopback): the server connection, if any, is closed.
void DisconnectServer();
// Where server keys are pinned (community::Options::knownServersFile).
std::string KnownServersFile();

} // namespace net
