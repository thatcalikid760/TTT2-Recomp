// Steam backend of the online layer (port/ONLINE_PLAN.md phase 4): App ID 480, peer packets over
// SteamNetworkingMessages (Valve relays, no port forwarding), sessions as Steam lobbies, a friend's
// "Join Game" as the game's own invite. Built only when the Steamworks SDK is present
// (TTT2_HAVE_STEAM); otherwise every call reports "not running" and the loopback backend is used.
// steam_api64.dll is delay-loaded: a missing DLL or a closed Steam client just means no Steam.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace steamnet {

using Receiver = void (*)(uint64_t fromPeer, bool reliable, const uint8_t* data, size_t size);

// Starts Steam once (idempotent): false when Steam is unavailable. `receiver` gets every packet
// on the service thread.
bool Start(Receiver receiver);
bool Running();
uint64_t LocalId();                      // our SteamID (0 when not running)
bool Send(uint64_t peer, bool reliable, const void* data, size_t size);

// Lobbies: one per hosted session, carrying `record` (opaque bytes) as lobby data, findable by
// searchers of the same port protocol version.
void PublishLobby(uint64_t sessionId, const std::vector<uint8_t>& record);
void RemoveLobby(uint64_t sessionId);
std::vector<std::vector<uint8_t>> ListLobbies(int max, int timeoutMs);
// A friend's "Join Game" (GameLobbyJoinRequested_t): the lobby's record, once.
bool TakeJoinRequest(std::vector<uint8_t>& record);

// The calls above run Steam in a HELPER PROCESS (steam_bridge.cpp): this exe started again as
// `--steam-bridge`, talking over its stdin/stdout. Steam Input hides every controller from the
// process that runs SteamAPI as app 480 (MEASURED 2026-10-12: XInput slots 0+1 connected before
// SteamAPI_InitEx, none after, gameoverlayrenderer64.dll loaded) and Spacewar's controller
// config gives the game nothing back - so the game process must never load Steam. Unhooked
// processes keep seeing the pads while app 480 runs. TTT2_STEAM_INPROC=1 = the old in-process
// path (diagnostics only: no controller).
int RunBridge();                         // main() of the helper process

// The in-process implementation (steam_net.cpp), used by the helper.
namespace direct {
bool Start(Receiver receiver);
bool Running();
uint64_t LocalId();
bool Send(uint64_t peer, bool reliable, const void* data, size_t size);
void PublishLobby(uint64_t sessionId, const std::vector<uint8_t>& record);
void RemoveLobby(uint64_t sessionId);
std::vector<std::vector<uint8_t>> ListLobbies(int max, int timeoutMs);
bool TakeJoinRequest(std::vector<uint8_t>& record);
} // namespace direct

} // namespace steamnet
