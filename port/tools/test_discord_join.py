"""Exercise the Discord Join logic with the production sources and no Discord.

discord_presence.cpp (built without the SDK) and matchmaking.cpp run against mocked game state,
transport and Steam: the join secret format, which sessions get a party, a Join that arrives
before a profile is signed in, the session lookup retries, a full / vanished / own session, and
the queued invite reaching mm::TakeInvite ahead of Steam's. Presence itself (the SDK thread)
needs a running Discord client and is tested by hand.
"""

from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]

# discord_presence.cpp with the game, matchmaking and the transport mocked.
PRESENCE_CHECKER = r'''
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "matchmaking.h"
#include "profiles.h"
#include "xlive.h"
std::atomic<bool> signedIn{false}, liveOn{true};
std::mutex mockMutex;
std::vector<mm::SessionRecord> listed;      // what mm::List returns
int listCalls=0, showsUpOnCall=0;           // >0: `listed` is visible from that call on
std::vector<mm::SessionRecord> queued;
bool profiles::SignedIn() { return signedIn; }
bool xlive::Enabled() { return liveOn; }
uint64_t xlive::LocalPeerId() { return 7; }
std::vector<mm::SessionRecord> mm::List(uint32_t) {
    std::lock_guard l(mockMutex);++listCalls;
    if(showsUpOnCall && listCalls<showsUpOnCall) return {};
    return listed;
}
bool mm::Find(uint64_t, mm::SessionRecord&) { return false; }
void mm::QueueInvite(const mm::SessionRecord& r) { std::lock_guard l(mockMutex);queued.push_back(r); }
@PRESENCE@
using namespace discord_presence;
static unsigned checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
mm::SessionRecord Session(uint64_t id,uint32_t filled,uint32_t max,uint32_t flags=0x2F,uint32_t gameType=1) {
    mm::SessionRecord r;r.id=id;r.flags=flags;r.filledPublic=filled;r.maxPublic=max;
    r.contexts.push_back({0x800A,gameType});r.contexts.push_back({0x800B,0});
    return r;
}
void Reset(int showsUp=0) { std::lock_guard l(mockMutex);listed.clear();queued.clear();listCalls=0;showsUpOnCall=showsUp; }
size_t Queued() { std::lock_guard l(mockMutex);return queued.size(); }
int ListCalls() { std::lock_guard l(mockMutex);return listCalls; }
bool WaitQueued(size_t n,int ms) {
    for(int i=0;i<ms/10;++i){ if(Queued()>=n) return true;std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return Queued()>=n;
}
int main() {
    using namespace detail;
    // Secrets: the session id, versioned; anything else is refused.
    uint64_t id=0;
    CHECK(SecretOf(0x8076B97C1F83AC5Eull)=="ttt2-1:8076B97C1F83AC5E");
    CHECK(PartyIdOf(0x8076B97C1F83AC5Eull)=="8076B97C1F83AC5E" && PartyIdOf(1)=="0000000000000001");
    CHECK(ParseSecret(SecretOf(0x8076B97C1F83AC5Eull),id) && id==0x8076B97C1F83AC5Eull);
    for(const char* bad : {"","8076B97C1F83AC5E","ttt2-1:8076b97c1f83ac5e","ttt2-1:8076B97C1F83AC5","ttt2-2:8076B97C1F83AC5E",
                           "ttt2-1:8076B97C1F83AC5E0","ttt2-1:0000000000000000","ttt2-1:8076B97C1F83AC5G"}) {
        id=42;CHECK(!ParseSecret(bad,id) && id==42);
    }
    // Parties only for a Player Match this machine hosts.
    CHECK(IsPlayerMatchHost(Session(1,1,2)));
    CHECK(!IsPlayerMatchHost(Session(1,1,2,0x2F,0)));          // ranked GAME_TYPE
    CHECK(!IsPlayerMatchHost(Session(1,1,2,0x3F)));            // arbitrated
    CHECK(!IsPlayerMatchHost(Session(1,1,2,0x32E)));           // joined, not hosted
    CHECK(!IsPlayerMatchHost(Session(1,0,0)));
    mm::SessionRecord bare=Session(1,1,2);bare.contexts.clear();CHECK(IsPlayerMatchHost(bare));

    // Without the SDK nothing waits: quitting returns at once.
    Start();
    HostSession(Session(0x77,1,2));
    auto t0=std::chrono::steady_clock::now();Shutdown();
    CHECK(std::chrono::steady_clock::now()-t0<std::chrono::milliseconds(100));

    // A Join before any profile is signed in waits, then finds the session and queues the invite.
    Reset();{std::lock_guard l(mockMutex);listed.push_back(Session(0x8000000000000ABCull,1,2));}
    OnJoinSecret(SecretOf(0x8000000000000ABCull));
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(Queued()==0 && ListCalls()==0);
    signedIn=true;
    CHECK(WaitQueued(1,3000));
    { std::lock_guard l(mockMutex);CHECK(queued[0].id==0x8000000000000ABCull && listCalls==1); }
    // Not listed yet (the host's lobby is still being created): retried until it shows up.
    Reset(3);{std::lock_guard l(mockMutex);listed.push_back(Session(0x8000000000000DEFull,2,4));}
    OnJoinSecret(SecretOf(0x8000000000000DEFull));
    CHECK(WaitQueued(1,8000) && ListCalls()==3);
    // A full session is not joined; neither is one that never shows up (the host left).
    Reset();{std::lock_guard l(mockMutex);listed.push_back(Session(0x8000000000000F00ull,2,2));}
    OnJoinSecret(SecretOf(0x8000000000000F00ull));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(Queued()==0 && ListCalls()==1);
    Reset();OnJoinSecret(SecretOf(0x8000000000000F01ull));
    std::this_thread::sleep_for(std::chrono::milliseconds(7500));
    CHECK(Queued()==0 && ListCalls()==5);
    // Our own hosted session, and secrets of other games, are ignored.
    Reset();HostSession(Session(0x8000000000000AAAull,1,2));
    {std::lock_guard l(mockMutex);listed.push_back(Session(0x8000000000000AAAull,1,2));}
    OnJoinSecret(SecretOf(0x8000000000000AAAull));OnJoinSecret("someothergame:1234");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    CHECK(Queued()==0 && ListCalls()==0);
    EndSession(0x8000000000000AAAull);
    // A newer Join replaces one still waiting for the sign-in.
    Reset();signedIn=false;
    {std::lock_guard l(mockMutex);listed.push_back(Session(0x8000000000000B01ull,1,2));listed.push_back(Session(0x8000000000000B02ull,1,2));}
    OnJoinSecret(SecretOf(0x8000000000000B01ull));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    OnJoinSecret(SecretOf(0x8000000000000B02ull));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    signedIn=true;
    CHECK(WaitQueued(1,3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    { std::lock_guard l(mockMutex);CHECK(queued.size()==1 && queued[0].id==0x8000000000000B02ull); }
    std::printf("PASS %u Discord join checks: secrets, Player Match parties, join before sign-in, lookup retries, full / gone / own sessions, newest join wins\n",checks);
}
'''

# matchmaking.cpp with the transport, Steam and presence mocked.
MM_CHECKER = r'''
#include <cstdio>
#include <deque>
#include <string>
#include <vector>
#include "matchmaking.h"
#include "net_transport.h"
#include "steam_net.h"
#include "discord_presence.h"
std::vector<uint64_t> hosted, ended;
std::deque<std::vector<uint8_t>> steamJoins;
const char* net::BackendName() { return "steam"; }
uint64_t net::LocalPeer() { return 7; }
void steamnet::PublishLobby(uint64_t, const std::vector<uint8_t>&) {}
void steamnet::RemoveLobby(uint64_t) {}
std::vector<std::vector<uint8_t>> steamnet::ListLobbies(int, int) { return {}; }
bool steamnet::TakeJoinRequest(std::vector<uint8_t>& r) {
    if(steamJoins.empty()) return false;r=steamJoins.front();steamJoins.pop_front();return true;
}
void discord_presence::HostSession(const mm::SessionRecord& r) { hosted.push_back(r.id); }
void discord_presence::EndSession(uint64_t id) { ended.push_back(id); }
@MM@
static unsigned checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
int main() {
    mm::SessionRecord own;own.id=0x10;own.hostPeer=7;own.maxPublic=2;own.filledPublic=1;
    mm::Publish(own);CHECK(hosted.size()==1 && hosted[0]==0x10);
    own.filledPublic=2;mm::Publish(own);CHECK(hosted.size()==2);    // the party fills
    mm::Remove(0x10);CHECK(ended.size()==1 && ended[0]==0x10);
    // A Discord Join reaches TakeInvite (and Find) like a Steam "Join Game", without hiding it.
    mm::SessionRecord found;
    CHECK(!mm::TakeInvite(found));
    mm::SessionRecord friendSession;friendSession.id=0x8000000000000ABCull;friendSession.hostPeer=99;friendSession.maxPublic=2;
    mm::SessionRecord steamSession=friendSession;steamSession.id=0x8000000000000DEFull;
    steamJoins.push_back(mm::Serialize(steamSession));                 // a Steam "Join Game" too
    mm::QueueInvite(friendSession);
    CHECK(mm::TakeInvite(found) && found.id==0x8000000000000ABCull);
    mm::SessionRecord again;CHECK(mm::Find(0x8000000000000ABCull,again) && again.hostPeer==99);
    CHECK(mm::TakeInvite(found) && found.id==0x8000000000000DEFull);   // Steam's still arrives
    CHECK(!mm::TakeInvite(found));                                     // each once
    std::printf("PASS %u matchmaking checks: presence follows Publish / Remove, a Discord Join reaches TakeInvite once, Steam joins unchanged\n",checks);
}
'''


# Production discord_presence.cpp against the real SDK (delay-loaded, as in the game), in the cases
# that must not touch Discord: turned off, no Application ID, DLL missing. (With an ID it would
# register a launch command for that ID in the registry: that is the hand test.)
SDK_CHECKER = r'''
#define TTT2_HAVE_DISCORD 1
#include <cstdio>
#include <vector>
#include "matchmaking.h"
#include "profiles.h"
#include "xlive.h"
bool profiles::SignedIn() { return false; }
bool xlive::Enabled() { return true; }
uint64_t xlive::LocalPeerId() { return 7; }
std::vector<mm::SessionRecord> mm::List(uint32_t) { return {}; }
bool mm::Find(uint64_t, mm::SessionRecord&) { return false; }
void mm::QueueInvite(const mm::SessionRecord&) {}
@PRESENCE@
int main() {
    discord_presence::Start();
    discord_presence::Shutdown();
    std::printf("loaded=%d\n", GetModuleHandleW(L"discord_partner_sdk.dll") != nullptr);
}
'''


def check_sdk(kernel, folder, presence):
    import os, shutil
    sdks = sorted((ROOT / "tools").glob("DiscordSocialSdk-*/discord_social_sdk")) + \
        [p for p in [ROOT / "tools/discord_social_sdk"] if p.is_dir()]
    sdks = [s for s in sdks if (s / "include/discordpp.h").is_file()]
    if not sdks:
        print("SKIP Discord SDK checks: no Discord Social SDK in tools/")
        return
    sdk = sdks[-1]
    exe = compile_one(folder, "sdk", SDK_CHECKER.replace("@PRESENCE@", presence), [kernel, sdk / "include"],
                      f'/link "{sdk / "lib/release/discord_partner_sdk.lib"}" delayimp.lib /DELAYLOAD:discord_partner_sdk.dll')
    shutil.copy(sdk / "bin/release/discord_partner_sdk.dll", folder / "discord_partner_sdk.dll")
    ini = folder / "discord_settings.ini"

    def run(exe_path):
        r = subprocess.run([str(exe_path)], timeout=30, capture_output=True, text=True,
                           env=dict(os.environ, TTT2_SETTINGS_INI=str(ini)))
        assert r.returncode == 0, r.stdout + r.stderr
        return r.stdout + r.stderr

    out = run(exe)            # fresh settings: on, but no Application ID
    assert "no Application ID" in out and "loaded=0" in out, out
    text = ini.read_text()
    assert "Enabled=1" in text and "ApplicationId=" in text, text
    ini.write_text("[Discord]\nEnabled=0\nApplicationId=123456789012345678\n")
    out = run(exe)
    assert "[discord] off ([Discord] Enabled=0)" in out and "loaded=0" in out, out
    alone = folder / "no_dll"
    alone.mkdir()
    shutil.copy(exe, alone / exe.name)
    ini.write_text("[Discord]\nEnabled=1\nApplicationId=123456789012345678\n")
    out = run(alone / exe.name)
    assert "discord_partner_sdk.dll not found" in out and "loaded=0" in out, out
    print(f"PASS Discord SDK checks ({sdk.parent.name}): off, no Application ID, DLL missing")


def main():
    kernel = ROOT / "port/src/runtime/kernel"
    presence = (kernel / "discord_presence.cpp").read_text(encoding="utf-8-sig")
    matchmaking = (kernel / "matchmaking.cpp").read_text(encoding="utf-8-sig")
    with tempfile.TemporaryDirectory(prefix="ttt2-discord-") as temp:
        folder = Path(temp)
        exe = compile_one(folder, "presence", PRESENCE_CHECKER.replace("@PRESENCE@", presence), [kernel])
        subprocess.run([str(exe)], check=True, timeout=60)
        exe = compile_one(folder, "mm", MM_CHECKER.replace("@MM@", matchmaking), [kernel])
        subprocess.run([str(exe)], check=True, timeout=60)
        check_sdk(kernel, folder, presence)


if __name__ == "__main__":
    main()
