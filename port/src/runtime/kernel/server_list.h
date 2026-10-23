// The community servers the player keeps and what they answer (port/COMMUNITY_SERVER_PLAN.md phase 2): the saved
// list and the chosen network in servers.ini (%LOCALAPPDATA%\TTT2Recomp\servers.ini; TTT2_SERVERS_INI = another
// file, for tests), and live INFO answers (players, ping, message) for the in-game server list - saved servers are
// asked every few seconds while the list is on screen, LAN servers are found by broadcast. INFO answers are
// information only: the TLS login (community_net.h) decides who a server is.
//
// No game-runtime dependencies; thread-safe.
#pragma once
#include "community_net.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace server_list {

constexpr size_t kMaxSaved = 32;
constexpr uint16_t kDefaultPort = 28200;

struct Saved
{
    std::string address;                  // as the player typed it: host[:port]
    std::string name;                     // as the server last reported it ("" = never heard)
    std::string password;                 // "" = none; stored encrypted for this Windows user
};

// The saved servers, in the player's order.
std::vector<Saved> List();
// Adds a server or updates the one with the same address; false when the list is full or the address is not one.
bool Save(const Saved& server);
void Remove(const std::string& address);
// The network ONLINE MODE plays on: "" = Steam, otherwise a server's address.
std::string Current();
void SetCurrent(const std::string& address);

// The same server? (addresses compared without case, the default port implied)
bool SameAddress(const std::string& a, const std::string& b);

enum class State { Unknown, Waiting, Answered, Offline, BadAddress };
struct Live
{
    State state = State::Unknown;
    community::ServerInfo info;           // the last answer (kept while a later question is open)
    int pingMs = -1;
    bool full = false;                    // the last answer carried the whole message of the day
    uint32_t answers = 0;                 // answers so far (a newer one has a higher count)
};

// While on, every saved server is asked every 3 s and the LAN is searched (the list is on screen).
void Watch(bool on);
// Servers that banned the player, or keep them out for a while after a kick - as their logins told this player
// (error 6, community_net.h): the list leaves them out until then. `until`: unix seconds, 0 = for good; a time already
// past (a kick without a block) forgets the server's ban. Kept in servers.ini ([Banned]).
void SetBanned(const std::string& address, uint64_t until);
bool Banned(const std::string& address, uint64_t* until = nullptr);
// The servers whose bans are in force (address, until: unix seconds, 0 = for good).
std::vector<std::pair<std::string, uint64_t>> Bans();
// Asks one address now, with the long request (the whole message of the day): ADD SERVER, SERVER INFO.
void Ask(const std::string& address);
Live Get(const std::string& address);

struct Found
{
    std::string address;                  // "a.b.c.d" (or "a.b.c.d:port" off the default port)
    Live live;
};
// LAN servers that answered the broadcast in the last 10 s, saved ones left out.
std::vector<Found> Lan();

// Changes whenever anything above does (the list redraws then).
uint32_t Version();

} // namespace server_list
