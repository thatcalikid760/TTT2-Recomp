// Ranked Match extras (port only; the game's own ranked flow is one match per room - after it both games delete the
// session and search again):
//  - Rematch, as in TEKKEN 8: after a ranked match that ended normally both players are asked "REMATCH?" (a note, top
//    right: Y / click accepts, X / right-click declines, 20 s). When both accept, the next ranked search pairs them
//    again - the player with the lower peer id hosts a room reserved for the other, who searches only for it. Once:
//    after the rematch the next search is a normal one.
//  - Quits: when the opponent's packets had stopped (3 s or more) by the time the game ends or deletes the ranked
//    session, the opponent left before the end; on a community server that keeps ranked conduct
//    (server/PROTOCOL.md "Ranked conduct") the game says so (RANKED_LEFT), the server judges the quit and gives the
//    quitter a ranked cooldown. The opponent's quit rate is shown when a ranked match is found and on the rematch
//    question; a player in a cooldown is told so when they search.
#pragma once
#include "matchmaking.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ranked {

// xsession.cpp (any thread; never under its lock calling back into it). `flags`: the session's XSessionCreate flags,
// `gameType`: X_CONTEXT_GAME_TYPE (0 ranked).
void SessionStarted(uint64_t session, uint32_t flags);
void SessionEnded(uint64_t session, uint32_t flags, uint32_t gameType);
void SessionDeleted(uint64_t session, uint32_t flags, uint32_t gameType);   // before the match peer is forgotten
void OpponentKnown(uint64_t opponent);                                       // a ranked match registers (arbitration)

// A ranked search (XSessionSearch with GAME_TYPE 0). `canWait`: it runs on the game's worker thread and may block (it
// waits for an open rematch question to be answered).
struct SearchPlan
{
    enum Kind { Normal, Host, Join } kind = Normal;   // Host: find nothing (the game hosts); Join: only `peer`'s room
    uint64_t peer = 0;
};
SearchPlan BeforeSearch(bool canWait);
void JoinNotFound();                                    // the rematch room never showed up: back to normal searches

// Session records: a rematch host's room carries the peer it waits for (two contexts); other searchers skip it.
void AddReservation(std::vector<mm::Context>& contexts);
bool ReservedForOther(const mm::SessionRecord& record);

void PeerMessage(uint64_t from, const uint8_t* data, size_t size);   // xsocket.cpp (transport thread)

// The game's main thread, every frame (menu_ext.cpp): the question's note and buttons, the server's notices.
void Frame();
uint16_t GameButtonMask();                               // Y / X are the question's while it is open
bool PromptShowing();

} // namespace ranked
