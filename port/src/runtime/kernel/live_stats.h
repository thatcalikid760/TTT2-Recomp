// Xbox LIVE statistics - the leaderboards (port/COMMUNITY_SERVER_PLAN.md phases 3-4): XamUserCreateStatsEnumerator
// (by rank, the only kind the game uses) and its XamEnumerate, XUserReadStats, XSessionWriteStats. On a community
// server that keeps statistics (WELCOME flag) the boards come from it and the game's writes go to it; otherwise
// every board is empty and writes are only logged.
#pragma once
#include <cstdint>

namespace live_stats {

// XamEnumerate / NtClose on a stats enumerator handle (0xD3xxxxxx); false for other handles. Enumerate waits for
// the server's page (asked for when the enumerator was created).
bool Enumerate(uint8_t* base, uint32_t handle, uint32_t buffer, uint32_t bytes, uint32_t& count, uint32_t& result);
bool Close(uint32_t handle);

// XUserReadStats {titleId, nXuids, pXuids, nSpecs, pSpecs, cbResults, pResults}: those players' rows (blocking on
// the server - xlive.cpp runs an overlapped one on a worker thread). Returns the HRESULT.
uint32_t ReadPlayers(uint8_t* base, uint32_t block);

// XSessionWriteStats {session, ?, u64 xuid, numViews, pViews} (`length` bytes at `buffer`) of the game's session
// `sessionId` (0 when not known): logged and handed to the server.
void Write(uint8_t* base, uint32_t buffer, uint32_t length, uint64_t sessionId);

} // namespace live_stats
