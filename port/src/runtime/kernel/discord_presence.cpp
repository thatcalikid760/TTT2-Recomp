// Discord Join / invite (see discord_presence.h). One thread owns the SDK client (its callbacks
// run there, from discordpp::RunCallbacks); the game's threads only change the desired presence
// and hand over join secrets, so nothing they call waits on Discord.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "discord_presence.h"
#include "profiles.h"
#include "xlive.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#if TTT2_HAVE_DISCORD
#define DISCORDPP_IMPLEMENTATION
#include "discordpp.h"
#endif

#ifndef TTT2_DISCORD_APP_ID
#define TTT2_DISCORD_APP_ID 0
#endif

namespace discord_presence {
namespace {

std::string IniPath()
{
    if (const char* e = std::getenv("TTT2_SETTINGS_INI"); e && *e) return e;   // test runs
    char dir[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir));
    std::string path = (n && n < sizeof(dir)) ? std::string(dir) : std::string(".");
    path += "\\TTT2Recomp";
    CreateDirectoryA(path.c_str(), nullptr);
    return path + "\\settings.ini";
}

// The presence the game wants (host thread writes, SDK thread applies by generation).
struct Desired
{
    bool hosting = false;
    uint64_t session = 0;
    uint32_t filled = 0, max = 0;
};
std::mutex g_mutex;
std::condition_variable g_cv;
Desired g_desired;
uint32_t g_generation = 0, g_applied = 0;
std::atomic<bool> g_started{ false };
std::atomic<bool> g_sdkRunning{ false };   // the SDK thread applies g_desired

// ---- joins: wait for the game, then find the session ----------------------------------------
uint64_t g_pendingJoin = 0;            // under g_mutex; a newer Join replaces an older one
bool g_resolver = false;

bool GameReady() { return xlive::Enabled() && profiles::SignedIn(); }

bool Resolve(uint64_t id)
{
    constexpr int kTries = 5;
    for (int attempt = 0; attempt < kTries; ++attempt)
    {
        {
            std::lock_guard l(g_mutex);
            if (g_pendingJoin != id) return false;           // superseded by a newer Join
        }
        if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        xlive::LocalPeerId();                                // the transport (Steam) must be up to list
        mm::SessionRecord found;
        bool have = false;
        for (mm::SessionRecord& r : mm::List(64))
            if (r.id == id) { found = std::move(r); have = true; break; }
        if (!have) have = mm::Find(id, found);               // loopback sessions, or one listed before
        if (!have)
        {
            std::fprintf(stderr, "[discord] Join: session %016llX not listed yet (try %d of %d)\n",
                         (unsigned long long)id, attempt + 1, kTries);
            continue;
        }
        const uint32_t slots = found.maxPublic + found.maxPrivate, filled = found.filledPublic + found.filledPrivate;
        if (slots && filled >= slots)
        {
            std::fprintf(stderr, "[discord] Join: session %016llX is full\n", (unsigned long long)id);
            return false;
        }
        std::fprintf(stderr, "[discord] Join: session %016llX found -> invite\n", (unsigned long long)id);
        mm::QueueInvite(found);
        return true;
    }
    std::fprintf(stderr, "[discord] Join: session %016llX not found (the host left?)\n", (unsigned long long)id);
    return false;
}

void Resolver()
{
    for (;;)
    {
        uint64_t id = 0;
        {
            std::unique_lock l(g_mutex);
            g_cv.wait(l, [] { return g_pendingJoin != 0; });
            id = g_pendingJoin;
        }
        // A Join that started the game waits for the title screen's profile sign-in.
        bool announced = false;
        while (!GameReady())
        {
            if (!announced) std::fprintf(stderr, "[discord] Join queued until a profile is signed in\n");
            announced = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::lock_guard l(g_mutex);
            if (g_pendingJoin != id) break;
        }
        Resolve(id);
        std::lock_guard l(g_mutex);
        if (g_pendingJoin == id) g_pendingJoin = 0;
    }
}

#if TTT2_HAVE_DISCORD
uint64_t ApplicationId()
{
    const std::string ini = IniPath();
    char value[32] = {};
    // Written out when absent, so the keys are easy to find.
    GetPrivateProfileStringA("Discord", "ApplicationId", "\x01", value, sizeof(value), ini.c_str());
    if (value[0] == '\x01')
    {
        WritePrivateProfileStringA("Discord", "ApplicationId", "", ini.c_str());
        value[0] = 0;
    }
    const uint64_t fromIni = std::strtoull(value, nullptr, 10);
    return fromIni ? fromIni : uint64_t(TTT2_DISCORD_APP_ID);
}

bool Enabled()
{
    const std::string ini = IniPath();
    char value[8] = {};
    GetPrivateProfileStringA("Discord", "Enabled", "", value, sizeof(value), ini.c_str());
    if (!*value)
    {
        WritePrivateProfileStringA("Discord", "Enabled", "1", ini.c_str());
        return true;
    }
    return std::strtol(value, nullptr, 10) != 0;
}

void Apply(discordpp::Client& client, const Desired& d)
{
    if (!d.hosting)
    {
        client.ClearRichPresence();
        std::fprintf(stderr, "[discord] presence cleared\n");
        return;
    }
    discordpp::Activity activity;
    activity.SetType(discordpp::ActivityTypes::Playing);
    discordpp::ActivityParty party;
    party.SetId(detail::PartyIdOf(d.session));
    party.SetCurrentSize(int32_t(d.filled));
    party.SetMaxSize(int32_t(d.max));
    party.SetPrivacy(discordpp::ActivityPartyPrivacy::Public);
    activity.SetParty(party);
    discordpp::ActivitySecrets secrets;
    secrets.SetJoin(detail::SecretOf(d.session));
    activity.SetSecrets(secrets);
    activity.SetSupportedPlatforms(discordpp::ActivityGamePlatforms::Desktop);
    const uint64_t session = d.session;
    client.UpdateRichPresence(std::move(activity), [session](discordpp::ClientResult result) {
        if (result.Successful())
            std::fprintf(stderr, "[discord] presence: hosting %016llX\n", (unsigned long long)session);
        else
            std::fprintf(stderr, "[discord] presence not set: %s\n", result.Error().c_str());
    });
}

void SdkThread(uint64_t appId)
{
    try
    {
        discordpp::Client client;
        client.AddLogCallback([](std::string message, discordpp::LoggingSeverity) {
            std::fprintf(stderr, "[discord] sdk: %s\n", message.c_str());
        }, discordpp::LoggingSeverity::Warning);
        client.SetApplicationId(appId);
        // Every startup, so Discord can start the game for a Join while it is closed. An empty
        // command registers this executable without arguments (argv[1] would be an image path).
        const bool registered = client.RegisterLaunchCommand(appId, "");
        std::fprintf(stderr, "[discord] launch command %s\n", registered ? "registered" : "NOT registered");
        client.SetActivityJoinCallback([](std::string secret) { OnJoinSecret(secret); });
        // Talk to the Discord client right away (a Join that started the game is delivered then).
        client.ClearRichPresence();
        g_sdkRunning = true;
        std::fprintf(stderr, "[discord] running (application %llu, no login: through the Discord client)\n",
                     (unsigned long long)appId);
        for (;;)
        {
            discordpp::RunCallbacks();
            Desired want;
            uint32_t generation = 0;
            {
                std::lock_guard l(g_mutex);
                want = g_desired;
                generation = g_generation;
            }
            if (generation != g_applied)
            {
                Apply(client, want);
                discordpp::RunCallbacks();
                std::lock_guard l(g_mutex);
                g_applied = generation;
                g_cv.notify_all();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    catch (...)
    {
        g_sdkRunning = false;
        std::fprintf(stderr, "[discord] SDK error - Discord presence stopped\n");
    }
}
#endif

void SetDesired(const Desired& d)
{
    std::lock_guard l(g_mutex);
    const Desired& c = g_desired;
    if (c.hosting == d.hosting && (!d.hosting || (c.session == d.session && c.filled == d.filled && c.max == d.max)))
        return;
    g_desired = d;
    ++g_generation;
}

} // namespace

void Start()
{
    if (g_started.exchange(true)) return;
#if TTT2_HAVE_DISCORD
    if (!Enabled())
    {
        std::fprintf(stderr, "[discord] off ([Discord] Enabled=0)\n");
        return;
    }
    const uint64_t appId = ApplicationId();
    if (!appId)
    {
        std::fprintf(stderr, "[discord] off: no Application ID ([Discord] ApplicationId in settings.ini)\n");
        return;
    }
    // Delay-loaded: never call into the SDK unless the DLL is really there.
    if (!LoadLibraryW(L"discord_partner_sdk.dll"))
    {
        std::fprintf(stderr, "[discord] discord_partner_sdk.dll not found - Discord Join unavailable\n");
        return;
    }
    std::thread(SdkThread, appId).detach();
#else
    std::fprintf(stderr, "[discord] not in this build (no Discord Social SDK)\n");
#endif
}

void HostSession(const mm::SessionRecord& r)
{
    if (detail::IsPlayerMatchHost(r))
    {
        SetDesired({ true, r.id, r.filledPublic + r.filledPrivate, r.maxPublic + r.maxPrivate });
        return;
    }
    std::unique_lock l(g_mutex);
    const bool ours = g_desired.hosting && g_desired.session == r.id;
    l.unlock();
    if (ours) SetDesired({});
}

void EndSession(uint64_t sessionId)
{
    std::unique_lock l(g_mutex);
    const bool ours = g_desired.hosting && g_desired.session == sessionId;
    l.unlock();
    if (ours) SetDesired({});
}

void Shutdown()
{
    SetDesired({});
    if (!g_sdkRunning) return;
    std::unique_lock l(g_mutex);
    g_cv.wait_for(l, std::chrono::milliseconds(300), [] { return g_applied == g_generation; });
}

void OnJoinSecret(const std::string& secret)
{
    uint64_t id = 0;
    if (!detail::ParseSecret(secret, id))
    {
        std::fprintf(stderr, "[discord] Join: not a session of this port version\n");
        return;
    }
    std::lock_guard l(g_mutex);
    if (g_desired.hosting && g_desired.session == id)
    {
        std::fprintf(stderr, "[discord] Join: that is our own session\n");
        return;
    }
    std::fprintf(stderr, "[discord] Join request: session %016llX\n", (unsigned long long)id);
    g_pendingJoin = id;
    if (!g_resolver) { g_resolver = true; std::thread(Resolver).detach(); }
    g_cv.notify_all();
}

} // namespace discord_presence
