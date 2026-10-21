// Discord Join / invite (Discord Social SDK), beside Steam's "Join Game" (steam_net.h), which it
// does not replace. While this player hosts a Player Match session, Discord shows a joinable
// party: id = the session id, size = filledPublic / maxPublic, join secret = the session id
// (XNKID) - not the Steam lobby id, which does not survive a Steam helper restart. Nothing else
// goes into the presence (no addresses, no paths).
//
// A friend's Join (with the game open, or started by Discord through the launch command
// registered at every startup) hands us that secret: once the game is ready (LIVE emulation on,
// a profile signed in) the session is looked up by id in the published sessions (a few tries)
// and fed to the game exactly like a Steam invite (mm::QueueInvite -> mm::TakeInvite ->
// XN_LIVE_INVITE_ACCEPTED). A join that arrives earlier waits for that.
//
// No Discord login: presence and Join go through the running Discord desktop client (the SDK's
// RPC path, without Client::Connect / OAuth). settings.ini:
//   [Discord] Enabled=0         turns it off
//   [Discord] ApplicationId=... overrides the build's TTT2_DISCORD_APP_ID (CMake cache; not in
//                               the repository)
// Without the SDK in the build, discord_partner_sdk.dll, an Application ID or a running Discord,
// nothing happens; no call here blocks the game.
#pragma once
#include "matchmaking.h"
#include <cstdint>
#include <string>

namespace discord_presence {

void Start();                                    // main(), once: reads settings, starts the SDK thread
void HostSession(const mm::SessionRecord& r);    // mm::Publish: show / update / clear the party
void EndSession(uint64_t sessionId);             // mm::Remove
void Shutdown();                                 // a normal quit: clear presence (waits <= 300 ms)

// A Join's secret (from the SDK's join callback). Public for the tests.
void OnJoinSecret(const std::string& secret);

namespace detail {
constexpr uint32_t kFlagHost = 0x1, kFlagArbitration = 0x10;   // XSessionCreate flags
constexpr uint32_t kContextGameType = 0x800A;                  // X_CONTEXT_GAME_TYPE, 0 = ranked

// A Player Match this machine hosts: not arbitrated, GAME_TYPE not ranked (0); TTT2 sets
// GAME_TYPE 1 for Player Match.
inline bool IsPlayerMatchHost(const mm::SessionRecord& r)
{
    if (!(r.flags & kFlagHost) || (r.flags & kFlagArbitration) || !r.maxPublic) return false;
    for (const mm::Context& c : r.contexts)
        if (c.id == kContextGameType && c.value == 0) return false;
    return true;
}

inline std::string PartyIdOf(uint64_t sessionId)
{
    static const char* d = "0123456789ABCDEF";
    std::string s(16, '0');
    for (int i = 0; i < 16; ++i) s[size_t(i)] = d[(sessionId >> (60 - 4 * i)) & 15];
    return s;
}
// "ttt2-1:" + the session id: secrets of other games or future versions are ignored.
inline std::string SecretOf(uint64_t sessionId) { return "ttt2-1:" + PartyIdOf(sessionId); }
inline bool ParseSecret(const std::string& secret, uint64_t& sessionId)
{
    static const std::string prefix = "ttt2-1:";
    if (secret.size() != prefix.size() + 16 || secret.compare(0, prefix.size(), prefix) != 0) return false;
    uint64_t id = 0;
    for (size_t i = prefix.size(); i < secret.size(); ++i)
    {
        const char c = secret[i];
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        id = id << 4 | uint64_t(v);
    }
    if (!id) return false;
    sessionId = id;
    return true;
}
} // namespace detail

} // namespace discord_presence
