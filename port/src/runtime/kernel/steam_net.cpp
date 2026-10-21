// Steam backend, in-process part (steam_net.h `direct`): runs inside the --steam-bridge helper
// process, never in the game process (Steam Input would hide the controllers; steam_bridge.cpp).
// One service thread owns Steam's manual callback dispatch: it runs
// the frame, routes call results to waiters (by SteamAPICall_t), accepts peer sessions, turns
// "Join Game" requests into pending invites, and drains SteamNetworkingMessages to the receiver.
// Other threads only issue requests and wait on the results it stores.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "steam_net.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#if TTT2_HAVE_STEAM
#include "steam/steam_api.h"
#include "steam/isteamnetworkingmessages.h"
#include "steam/isteamnetworkingutils.h"

namespace steamnet::direct {
namespace {

constexpr const char* kProtocol = "ttt2-port-1";          // lobbies of other builds / games are ignored
constexpr int kChannel = 0;

std::atomic<bool> g_running{ false };
std::atomic<uint64_t> g_local{ 0 };
Receiver g_receiver = nullptr;
HSteamPipe g_pipe = 0;

std::mutex g_mutex;
std::condition_variable g_cv;
std::map<SteamAPICall_t, std::vector<uint8_t>> g_results;      // finished call results
std::map<uint64_t, bool> g_lobbyData;                            // lobby -> data update arrived
struct Hosted { uint64_t lobby = 0; SteamAPICall_t pending = 0; std::vector<uint8_t> record; bool removed = false; };
std::map<uint64_t, Hosted> g_hosted;                             // session id -> its lobby
uint64_t g_joinLobby = 0;                                        // a "Join Game" request in progress
std::vector<uint8_t> g_joinRecord;
bool g_joinReady = false;

std::string Hex(const std::vector<uint8_t>& b)
{
    static const char* d = "0123456789ABCDEF";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t x : b) { s.push_back(d[x >> 4]); s.push_back(d[x & 15]); }
    return s;
}
std::vector<uint8_t> Unhex(const char* s)
{
    std::vector<uint8_t> b;
    auto v = [](char c) { return c >= 'A' ? (c & 7) + 9 : c - '0'; };
    for (size_t i = 0; s && s[i] && s[i + 1]; i += 2) b.push_back(uint8_t(v(s[i]) << 4 | v(s[i + 1])));
    return b;
}

void WriteLobbyData(uint64_t lobby, const std::vector<uint8_t>& record)
{
    CSteamID id(lobby);
    SteamMatchmaking()->SetLobbyData(id, "proto", kProtocol);
    SteamMatchmaking()->SetLobbyData(id, "rec", Hex(record).c_str());
}

// Service thread: callbacks, call results, packets.
void Service()
{
    std::vector<SteamNetworkingMessage_t*> messages(64);
    for (;;)
    {
        SteamAPI_ManualDispatch_RunFrame(g_pipe);
        CallbackMsg_t cb;
        while (SteamAPI_ManualDispatch_GetNextCallback(g_pipe, &cb))
        {
            if (cb.m_iCallback == SteamAPICallCompleted_t::k_iCallback)
            {
                const auto* done = reinterpret_cast<const SteamAPICallCompleted_t*>(cb.m_pubParam);
                std::vector<uint8_t> result(done->m_cubParam);
                bool failed = false;
                if (SteamAPI_ManualDispatch_GetAPICallResult(g_pipe, done->m_hAsyncCall, result.data(), int(result.size()),
                                                             done->m_iCallback, &failed) && !failed)
                {
                    std::lock_guard l(g_mutex);
                    // A hosted lobby being created: store its id and write the data.
                    bool hostedCall = false;
                    for (auto& [sid, h] : g_hosted)
                        if (h.pending == done->m_hAsyncCall)
                        {
                            hostedCall = true;
                            h.pending = 0;
                            const auto* created = reinterpret_cast<const LobbyCreated_t*>(result.data());
                            if (created->m_eResult == k_EResultOK)
                            {
                                h.lobby = created->m_ulSteamIDLobby;
                                if (h.removed) SteamMatchmaking()->LeaveLobby(CSteamID(h.lobby));
                                else WriteLobbyData(h.lobby, h.record);
                                std::fprintf(stderr, "[steam] lobby %016llX hosts session %016llX\n",
                                             (unsigned long long)h.lobby, (unsigned long long)sid);
                            }
                            else
                                std::fprintf(stderr, "[steam] CreateLobby failed (%d)\n", int(created->m_eResult));
                        }
                    if (!hostedCall) g_results[done->m_hAsyncCall] = std::move(result);
                    g_cv.notify_all();
                }
            }
            else if (cb.m_iCallback == SteamNetworkingMessagesSessionRequest_t::k_iCallback)
            {
                const auto* req = reinterpret_cast<const SteamNetworkingMessagesSessionRequest_t*>(cb.m_pubParam);
                SteamNetworkingMessages()->AcceptSessionWithUser(req->m_identityRemote);
            }
            else if (cb.m_iCallback == SteamNetworkingMessagesSessionFailed_t::k_iCallback)
            {
                const auto* f = reinterpret_cast<const SteamNetworkingMessagesSessionFailed_t*>(cb.m_pubParam);
                std::fprintf(stderr, "[steam] session with %016llX failed: %s\n",
                             (unsigned long long)f->m_info.m_identityRemote.GetSteamID64(), f->m_info.m_szEndDebug);
            }
            else if (cb.m_iCallback == GameLobbyJoinRequested_t::k_iCallback)
            {
                const auto* j = reinterpret_cast<const GameLobbyJoinRequested_t*>(cb.m_pubParam);
                std::lock_guard l(g_mutex);
                g_joinLobby = j->m_steamIDLobby.ConvertToUint64();
                g_joinReady = false;
                SteamMatchmaking()->RequestLobbyData(j->m_steamIDLobby);
                std::fprintf(stderr, "[steam] Join Game request: lobby %016llX\n", (unsigned long long)g_joinLobby);
            }
            else if (cb.m_iCallback == LobbyDataUpdate_t::k_iCallback)
            {
                const auto* u = reinterpret_cast<const LobbyDataUpdate_t*>(cb.m_pubParam);
                std::lock_guard l(g_mutex);
                g_lobbyData[u->m_ulSteamIDLobby] = u->m_bSuccess != 0;
                if (u->m_ulSteamIDLobby == g_joinLobby && u->m_bSuccess)
                {
                    const CSteamID lobby(g_joinLobby);
                    const char* proto = SteamMatchmaking()->GetLobbyData(lobby, "proto");
                    if (proto && std::strcmp(proto, kProtocol) == 0)
                    {
                        g_joinRecord = Unhex(SteamMatchmaking()->GetLobbyData(lobby, "rec"));
                        g_joinReady = !g_joinRecord.empty();
                    }
                    else
                        std::fprintf(stderr, "[steam] Join Game: not a session of this port version\n");
                    g_joinLobby = 0;
                }
                g_cv.notify_all();
            }
            SteamAPI_ManualDispatch_FreeLastCallback(g_pipe);
        }
        // Packets: first byte = reliable flag (as the loopback backend), then the packet.
        for (;;)
        {
            const int n = SteamNetworkingMessages()->ReceiveMessagesOnChannel(kChannel, messages.data(), int(messages.size()));
            if (n <= 0) break;
            for (int i = 0; i < n; ++i)
            {
                SteamNetworkingMessage_t* m = messages[size_t(i)];
                const auto* data = static_cast<const uint8_t*>(m->GetData());
                if (g_receiver && m->GetSize() >= 1)
                    g_receiver(m->m_identityPeer.GetSteamID64(), data[0] != 0, data + 1, size_t(m->GetSize() - 1));
                m->Release();
            }
        }
        Sleep(1);
    }
}

// Wait for a call result of type T (on any thread); false on timeout.
template <typename T> bool Await(SteamAPICall_t call, T& out, int timeoutMs)
{
    std::unique_lock l(g_mutex);
    if (!g_cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return g_results.count(call) != 0; })) return false;
    const std::vector<uint8_t> r = std::move(g_results[call]);
    g_results.erase(call);
    if (r.size() < sizeof(T)) return false;
    std::memcpy(&out, r.data(), sizeof(T));
    return true;
}

} // namespace

bool Start(Receiver receiver)
{
    static std::mutex m;
    std::lock_guard l(m);
    if (g_running) return true;
    static bool tried = false;
    if (tried) return false;
    tried = true;
    if (!LoadLibraryW(L"steam_api64.dll"))
    {
        std::fprintf(stderr, "[steam] steam_api64.dll not found - Steam online unavailable\n");
        return false;
    }
    SetEnvironmentVariableW(L"SteamAppId", L"480");
    SetEnvironmentVariableW(L"SteamGameId", L"480");
    SteamErrMsg error = {};
    if (SteamAPI_InitEx(&error) != k_ESteamAPIInitResult_OK)
    {
        std::fprintf(stderr, "[steam] not available: %s\n", error);
        return false;
    }
    SteamAPI_ManualDispatch_Init();
    g_pipe = SteamAPI_GetHSteamPipe();
    SteamNetworkingUtils()->InitRelayNetworkAccess();
    // Relay (SDR) only, no ICE: with the user's direct-connection setting, ICE gathering trips a
    // Steam assertion ("candidate type 0x400, but 0x202 is allowed") whose handler kills this process.
    SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable,
                                                      k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable);
    g_receiver = receiver;
    g_local = SteamUser()->GetSteamID().ConvertToUint64();
    g_running = true;
    std::thread(Service).detach();
    std::fprintf(stderr, "[steam] running as %s (%016llX), app 480\n", SteamFriends()->GetPersonaName(),
                 (unsigned long long)g_local.load());
    return true;
}

bool Running() { return g_running.load(); }
uint64_t LocalId() { return g_local.load(); }

bool Send(uint64_t peer, bool reliable, const void* data, size_t size)
{
    if (!g_running || size + 1 > 512 * 1024) return false;
    thread_local std::vector<uint8_t> packet;
    packet.resize(size + 1);
    packet[0] = reliable ? 1 : 0;
    if (size) std::memcpy(packet.data() + 1, data, size);
    SteamNetworkingIdentity id;
    id.SetSteamID64(peer);
    const int flags = (reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoNagle) |
                      k_nSteamNetworkingSend_AutoRestartBrokenSession;
    return SteamNetworkingMessages()->SendMessageToUser(id, packet.data(), uint32_t(packet.size()), flags, kChannel) == k_EResultOK;
}

void PublishLobby(uint64_t sessionId, const std::vector<uint8_t>& record)
{
    if (!g_running) return;
    std::lock_guard l(g_mutex);
    Hosted& h = g_hosted[sessionId];
    h.record = record;
    h.removed = false;
    if (h.lobby) WriteLobbyData(h.lobby, record);
    else if (!h.pending) h.pending = SteamMatchmaking()->CreateLobby(k_ELobbyTypePublic, 8);
}

void RemoveLobby(uint64_t sessionId)
{
    if (!g_running) return;
    std::lock_guard l(g_mutex);
    const auto it = g_hosted.find(sessionId);
    if (it == g_hosted.end()) return;
    if (it->second.lobby) { SteamMatchmaking()->LeaveLobby(CSteamID(it->second.lobby)); g_hosted.erase(it); }
    else it->second.removed = true;                       // leave once the creation completes
}

std::vector<std::vector<uint8_t>> ListLobbies(int max, int timeoutMs)
{
    std::vector<std::vector<uint8_t>> out;
    if (!g_running) return out;
    SteamMatchmaking()->AddRequestLobbyListStringFilter("proto", kProtocol, k_ELobbyComparisonEqual);
    SteamMatchmaking()->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
    SteamMatchmaking()->AddRequestLobbyListResultCountFilter(max);
    const SteamAPICall_t call = SteamMatchmaking()->RequestLobbyList();
    LobbyMatchList_t list = {};
    if (!Await(call, list, timeoutMs)) { std::fprintf(stderr, "[steam] lobby list timed out\n"); return out; }
    for (uint32_t i = 0; i < list.m_nLobbiesMatching; ++i)
    {
        const CSteamID lobby = SteamMatchmaking()->GetLobbyByIndex(int(i));
        std::vector<uint8_t> rec = Unhex(SteamMatchmaking()->GetLobbyData(lobby, "rec"));
        if (!rec.empty()) out.push_back(std::move(rec));
    }
    std::fprintf(stderr, "[steam] lobby list: %u lobbies, %zu sessions\n", list.m_nLobbiesMatching, out.size());
    return out;
}

bool TakeJoinRequest(std::vector<uint8_t>& record)
{
    std::lock_guard l(g_mutex);
    if (!g_joinReady) return false;
    g_joinReady = false;
    record = std::move(g_joinRecord);
    return true;
}

} // namespace steamnet::direct

#else  // no Steamworks SDK in this build

namespace steamnet::direct {
bool Start(Receiver) { return false; }
bool Running() { return false; }
uint64_t LocalId() { return 0; }
bool Send(uint64_t, bool, const void*, size_t) { return false; }
void PublishLobby(uint64_t, const std::vector<uint8_t>&) {}
void RemoveLobby(uint64_t) {}
std::vector<std::vector<uint8_t>> ListLobbies(int, int) { return {}; }
bool TakeJoinRequest(std::vector<uint8_t>&) { return false; }
} // namespace steamnet::direct

#endif
