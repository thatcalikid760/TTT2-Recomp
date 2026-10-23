// Online voice chat (see voice_chat.h).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h>
#include <shellapi.h>
#include "voice_chat.h"
#include "display_settings.h"
#include "steam_net.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace voice_chat {
namespace {

using clock = std::chrono::steady_clock;
constexpr auto kPeerTimeout = std::chrono::seconds(5);      // no game packets for this long: not in our session
constexpr auto kTalkHold = std::chrono::milliseconds(300);
constexpr size_t kMaxQueued = 12;                            // buffers per player (~ a quarter second)

struct Peer
{
    clock::time_point lastPacket{}, lastVoice{}, met{};
    std::string name;
};

std::mutex g_mutex;
std::map<uint64_t, Peer> g_peers;
std::atomic<bool> g_localTalking{ false }, g_allMuted{ false };

// ---- playback: one XAudio2 source voice per player (created at its first packet)
struct Playback
{
    IXAudio2SourceVoice* voice = nullptr;
    uint32_t rate = 0;
    std::deque<std::vector<int16_t>> buffers;                // alive until XAudio2 has played them
};
std::mutex g_audioMutex;
IXAudio2* g_audio = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
std::map<uint64_t, Playback> g_playback;
bool g_audioFailed = false;

bool EnsureAudio()
{
    if (g_audio || g_audioFailed) return g_audio != nullptr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = XAudio2Create(&g_audio);
    if (SUCCEEDED(hr)) hr = g_audio->CreateMasteringVoice(&g_master);
    if (FAILED(hr))
    {
        std::fprintf(stderr, "[voice] no audio output for voice chat (%08lX)\n", static_cast<unsigned long>(hr));
        if (g_audio) g_audio->Release();
        g_audio = nullptr;
        g_audioFailed = true;
        return false;
    }
    return true;
}

void Play(uint64_t peer, const int16_t* samples, size_t count, uint32_t rate)
{
    {
        std::lock_guard l(g_mutex);
        Peer& p = g_peers[peer];
        if (p.met == clock::time_point{}) p.met = clock::now();
        p.lastVoice = clock::now();
    }
    const display_settings::Voice settings = display_settings::GetVoice();
    if (settings.mode == display_settings::VoiceOff || g_allMuted.load() || display_settings::IsVoiceMuted(peer) || !count)
        return;
    std::lock_guard l(g_audioMutex);
    if (!EnsureAudio()) return;
    Playback& pb = g_playback[peer];
    if (pb.voice && pb.rate != rate) { pb.voice->DestroyVoice(); pb.voice = nullptr; pb.buffers.clear(); }
    if (!pb.voice)
    {
        WAVEFORMATEX f{};
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = 1;
        f.nSamplesPerSec = rate;
        f.wBitsPerSample = 16;
        f.nBlockAlign = 2;
        f.nAvgBytesPerSec = rate * 2;
        if (FAILED(g_audio->CreateSourceVoice(&pb.voice, &f)) || !pb.voice) { pb.voice = nullptr; return; }
        pb.rate = rate;
        pb.voice->Start();
        std::fprintf(stderr, "[voice] playing %016llX (%u Hz)\n", (unsigned long long)peer, rate);
    }
    pb.voice->SetVolume(float(settings.volume) / 10.0f);
    XAUDIO2_VOICE_STATE state{};
    pb.voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    while (pb.buffers.size() > state.BuffersQueued) pb.buffers.pop_front();   // played: free
    if (state.BuffersQueued >= kMaxQueued) return;                          // far behind: drop, keep latency low
    pb.buffers.emplace_back(samples, samples + count);
    XAUDIO2_BUFFER b{};
    b.AudioBytes = UINT32(count * 2);
    b.pAudioData = reinterpret_cast<const BYTE*>(pb.buffers.back().data());
    if (FAILED(pb.voice->SubmitSourceBuffer(&b))) pb.buffers.pop_back();
}

bool GameInFront()
{
    DWORD process = 0;
    const HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &process);
    return w && process == GetCurrentProcessId();
}

// Every 20 ms: the microphone, the peers our voice goes to, F8.
void Tick()
{
    bool recording = false, f8 = false;
    std::vector<uint64_t> sentPeers;
    // TTT2_VOICE_FAKE_PEER=<SteamID64 hex> (tests): that player counts as in our session.
    static const uint64_t fake = [] { const char* e = std::getenv("TTT2_VOICE_FAKE_PEER"); return e ? std::strtoull(e, nullptr, 16) : 0ull; }();
    for (;;)
    {
        if (fake) NotePeer(fake);
        const display_settings::Voice settings = display_settings::GetVoice();
        const bool front = GameInFront();
        const bool f8Now = front && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        if (f8Now && !f8)
        {
            g_allMuted = !g_allMuted.load();
            std::fprintf(stderr, "[voice] F8: everyone %s\n", g_allMuted.load() ? "muted" : "heard again");
        }
        f8 = f8Now;
        std::vector<uint64_t> peers;
        {
            std::lock_guard l(g_mutex);
            const auto now = clock::now();
            for (const auto& [id, p] : g_peers)
                if (now - p.lastPacket < kPeerTimeout) peers.push_back(id);
        }
        if (peers != sentPeers)
        {
            sentPeers = peers;
            steamnet::VoicePeers(peers);
            std::fprintf(stderr, "[voice] %zu voice peer(s)\n", peers.size());
        }
        bool record = false;
        static const bool echo = [] { const char* e = std::getenv("TTT2_VOICE_ECHO"); return e && *e == '1'; }();
        if (!peers.empty() || echo)                      // TTT2_VOICE_ECHO=1: steam_net.cpp
        {
            if (settings.mode == display_settings::VoiceOpenMic) record = true;
            else if (settings.mode == display_settings::VoicePushToTalk)
                record = front && (GetAsyncKeyState(settings.pushToTalkKey) & 0x8000) != 0;
        }
        if (record != recording)
        {
            recording = record;
            steamnet::VoiceRecord(record);
            if (!record) g_localTalking = false;
        }
        Sleep(20);
    }
}

} // namespace

void Install()
{
    static std::once_flag once;
    std::call_once(once, [] {
        steamnet::VoiceHandlers h;
        h.pcm = Play;
        h.talking = [](bool on) { g_localTalking = on; };
        h.name = [](uint64_t peer, const std::string& name) {
            std::lock_guard l(g_mutex);
            g_peers[peer].name = name;
            std::fprintf(stderr, "[voice] %016llX is '%s'\n", (unsigned long long)peer, name.c_str());
        };
        steamnet::VoiceSetHandlers(h);
        std::thread(Tick).detach();
    });
}

void NotePeer(uint64_t peer)
{
    std::lock_guard l(g_mutex);
    Peer& p = g_peers[peer];
    p.lastPacket = clock::now();
    if (p.met == clock::time_point{}) p.met = p.lastPacket;
}

std::vector<Player> Players()
{
    std::vector<std::pair<clock::time_point, Player>> all;
    {
        std::lock_guard l(g_mutex);
        const auto now = clock::now();
        for (const auto& [id, p] : g_peers)
        {
            Player pl;
            pl.peer = id;
            pl.name = p.name.empty() ? "PLAYER" : p.name;
            pl.talking = now - p.lastVoice < kTalkHold;
            pl.inSession = now - p.lastPacket < kPeerTimeout;
            all.push_back({ (std::max)(p.lastPacket, p.lastVoice), pl });
        }
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<Player> out;
    for (auto& [t, p] : all)
    {
        if (out.size() == 6) break;
        p.muted = display_settings::IsVoiceMuted(p.peer);
        out.push_back(std::move(p));
    }
    return out;
}

bool LocalTalking() { return g_localTalking.load(); }

bool RemoteTalking()
{
    if (g_allMuted.load()) return false;
    std::vector<uint64_t> talking;
    {
        std::lock_guard l(g_mutex);
        const auto now = clock::now();
        for (const auto& [id, p] : g_peers)
            if (now - p.lastVoice < kTalkHold) talking.push_back(id);
    }
    for (uint64_t id : talking)
        if (!display_settings::IsVoiceMuted(id)) return true;
    return false;
}

bool AllMuted() { return g_allMuted.load(); }

void OpenSteamProfile(uint64_t peer)
{
    const std::wstring id = std::to_wstring(peer);
    const std::wstring steam = L"steam://url/SteamIDPage/" + id;
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", steam.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32)
    {
        std::fprintf(stderr, "[voice] Steam profile %016llX opened in Steam\n", (unsigned long long)peer);
        return;
    }
    const std::wstring web = L"https://steamcommunity.com/profiles/" + id;
    ShellExecuteW(nullptr, L"open", web.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    std::fprintf(stderr, "[voice] Steam profile %016llX opened in the browser\n", (unsigned long long)peer);
}

} // namespace voice_chat
