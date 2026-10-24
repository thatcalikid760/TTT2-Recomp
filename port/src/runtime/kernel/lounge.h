// ONLINE MODE -> FIGHTING LOUNGE (TTT2 Fighting Lounge beta 0.0.1, a work in progress toward a 3D lounge - see
// FIGHTING_LOUNGE.md): a room the players of the network share, with
// 20 cabinets (01..20) and a text chat for everyone in it. A cabinet seats two players and keeps a queue; when both
// seated players are ready, their games play an unranked PLAYER MATCH against each other (ranked::PlanLoungeMatch: the next
// QUICK MATCH search pins them together, the online menus are walked there by button pulses), then go back to the lounge.
// The winner stays seated and the next queued player takes the loser's seat (the loser joins the back of the queue).
//
// One game hosts the lounge: it publishes a session record marked as a lounge (never one the game's own searches
// find) and decides every seat and queue; the others send it requests and get its whole state back. Messages go
// over the transport's reliable channel (xsocket.cpp Op::Lounge). Beta limits: one lounge per network (the first
// game in hosts it), and the lounge closes when its host leaves it.
//
// The page is a copy of the online menus' Search Session page (menu_doc.cpp AddLoungeFrame, as the server list):
// row 1 LOUNGE CHAT (A: type a message), rows 2..21 the cabinets (A: sit / queue / ready, X: leave), 8 on screen
// and scrolled here past either end.
#pragma once
#include "kernel_shared.h"
#include "matchmaking.h"
#include <cstddef>
#include <cstdint>

namespace lounge {

// The transport thread: a lounge message from `from` (xsocket.cpp).
void PeerMessage(uint64_t from, const uint8_t* data, size_t size);
// The game's main thread, every frame (menu_ext.cpp UiFrame): the page, the lounge's upkeep, the walk to a match.
void Frame(PPCContext& ctx, uint8_t* base);
// The buttons the game may see (0xFFFF = all): the page answers A / X itself and scrolls at its ends.
uint16_t GameButtonMask();
// The page's key hints, in front of its own (menu_ext.cpp NativeLoungeKeyInfo).
void KeyHints(PPCContext& ctx, uint8_t* base);
// A session record that is a lounge's (xsession.cpp keeps it out of the game's searches).
bool IsLoungeRecord(const mm::SessionRecord& record);
// A lounge cabinet's match is over (ranked_match.cpp; `ended`: XSessionEnd, else deleted before its end).
void MatchOver(bool ended);
// The match's end (gin_trace.cpp, the ghost step): the side that won.
void MatchWinner(int side);

} // namespace lounge
