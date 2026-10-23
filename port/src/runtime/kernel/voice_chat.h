// Online voice chat (Options -> VOICE CHAT; display_settings::Voice). Steam records, compresses and
// carries the voice (steam_net.h); this side decides when the microphone is open (OPEN MIC, or
// PUSH TO TALK while the key is held with the game in front), who hears it (every peer the game
// exchanged packets with in the last few seconds: the lobby and the match alike), and plays each
// player through an XAudio2 voice of its own at VOICE VOLUME unless muted. F8 mutes everyone
// until pressed again. Only over Steam: the loopback backend has no voice.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace voice_chat {

// Hands the voice callbacks to the Steam layer (before it starts) and starts the voice thread.
void Install();
// A game packet went to or came from `peer` over Steam.
void NotePeer(uint64_t peer);

struct Player
{
    uint64_t peer = 0;
    std::string name;           // Steam name ("PLAYER" until Steam knows it)
    bool muted = false;
    bool talking = false;
    bool inSession = false;     // game packets from them in the last few seconds
};
// The players met this session (most recent first, at most 6).
std::vector<Player> Players();

// For the overlay: our microphone is sending / somebody unmuted is heard / F8 mutes everyone.
bool LocalTalking();
bool RemoteTalking();
bool AllMuted();

// Opens `peer`'s Steam profile in the Steam client (the web page when steam:// is not handled).
void OpenSteamProfile(uint64_t peer);

} // namespace voice_chat
