// The port's Xbox LIVE (port/ONLINE_PLAN.md): the signed-in profile is on LIVE (with the title
// storage emulation, TitleServerEnabled), XNet is up, and the game's Live services are answered
// here - sessions and stats in xsession.cpp, peer traffic through the transport (phase 3).
#pragma once
#include <cstddef>
#include <cstdint>

namespace xlive {

constexpr uint32_t kTitleId = 0x4E4D0859u;

bool Enabled();                         // LIVE emulation on (follows TitleServerEnabled)
uint64_t LocalPeerId();                 // this machine on the transport (net_transport.h; starts it)
uint64_t OnlineXuid(uint32_t user = 0);  // user 0's (the profile's) or a guest's online XUID (0 when signed out)
void WriteOwnXnAddr(uint8_t* base, uint32_t xnaddr);   // our XNADDR (0x24 bytes)

// Virtual addresses (host order) <-> peers: our own is 10.77.x.y (127.0.0.1 also means us), each
// remote peer gets 10.78.x.y on first sight. PeerOfAddr gives 0 for an unknown address.
uint32_t AddrOfPeer(uint64_t peer);
uint64_t PeerOfAddr(uint32_t addr);

// Random bytes (XNetRandom, session ids, nonces).
void Random(void* out, size_t bytes);

// Guest memory for results the title does not supply a buffer for (QoS results); never freed
// while the title may still read it.
uint32_t GuestAlloc(uint32_t bytes);

} // namespace xlive

// XMsg app dispatch (xlive.cpp): app 0xFB XGI (xsession.cpp), 0xFC XLiveBase, 0xFE XamApp. Returns
// the handler's HRESULT; `handled` false when the message is not ours.
uint32_t XLiveDispatch(uint8_t* base, uint32_t app, uint32_t msg, uint32_t buffer, uint32_t length, bool& handled);
uint32_t XgiDispatch(uint8_t* base, uint32_t msg, uint32_t buffer, uint32_t length, bool& handled);
// The calling thread is xlive.cpp's worker for an asynchronous request: a search on it may wait (xsession.cpp).
void XSessionSearchIsAsync(bool on);

// Enumerators created by XamCreateEnumeratorHandle / Live messages (handles 0xD2xxxxxx):
// XamEnumerate and NtClose hand them here. Item lists are empty for now (no friends, no
// title servers, no marketplace offers).
bool XLiveEnumerate(uint8_t* base, uint32_t handle, uint32_t buffer, uint32_t bytes, uint32_t& count, uint32_t& result);
bool XLiveCloseHandle(uint32_t handle, uint32_t& status);

// Starts the packet transport with the socket layer's receiver (xsocket.cpp); idempotent.
bool XSocketStartTransport();
// The peer of the match being played (the last one whose game datagrams arrived; 0: none yet).
uint64_t XSocketMatchPeer();
void XSocketForgetMatchPeer();                 // a session ended (XSessionDelete)

// Polled from XNotifyGetNext: a Steam "Join Game" or a Discord Join becomes XN_LIVE_INVITE_ACCEPTED, answered by
// XInviteGetAcceptedInfo with that session.
void XLivePollInvites();
