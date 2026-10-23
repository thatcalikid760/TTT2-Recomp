// Namco's GIN web service for the game (port/COMMUNITY_SERVER_PLAN.md phase 3, gin_service.cpp): the "TTT2" title
// server the game reaches through XTitleServerCreateEnumerator / XNetServerToInAddr is a service peer of the
// socket layer. Its requests go to the community server that is the network, or are answered here (Steam: no
// teams, ghosts or replays, no news) so the online modes open without the old "service has not started" notice.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "xsocket_service.h"

namespace gin_service {

constexpr uint32_t kGinService = 0x483207D3u;                 // the "TTT2" title server's service id
constexpr uint64_t kGinPeer = xsvc::kServicePeer | kGinService;

// GIN is switched on in the title storage the game reads at boot (with the server list; TTT2_SRV_UI=0 turns both
// off, TTT2_GIN_STUB=1 keeps it on alone).
bool Enabled();
void Start();                                                  // registers the service peer (idempotent)

// The news of the community server that is the network (its GIN settings: English news lines, else its message of
// the day), shown in the information bar between the game's tips (menu_ext.cpp); empty on Steam.
std::vector<std::string> News();

} // namespace gin_service
