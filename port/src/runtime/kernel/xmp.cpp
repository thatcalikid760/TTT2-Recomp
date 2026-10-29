// XMP (XAM app 0xFA, the music player / custom soundtracks), docs/tekken_tunes_plan.md. The XDK
// wrappers (ppc_recomp.168.cpp, 0x82851D28-0x82852A48) build a parameter block on their stack and
// send XMsgStartIORequest(Ex) or XMsgInProcessCall with r3 = 250.
//  - Offline answer (today's): XMPGetMediaSources (0x7002B) reports no sources, so TEKKEN TUNES
//    never gets further. Asking it on the way in and failing left the screen black ~27 s (the
//    user's run, 2026-10-15). Every other StartIO message is unserved (E_FAIL); InProcess messages
//    return 0, except 0x7001B (XMPGetPlaybackController) which keeps 0xFFFFFFFF.
//  - Task 1 of the plan: every app-0xFA message is logged (the first 200 of each), and env
//    TTT2_TUNES_STUB=1 reports one ready source with three silent songs ("Stub A/B/C", group
//    "Stub") so the menu, save and fight traffic can be recorded. No audio.
#include "kernel_shared.h"
#include "kernel_content.h"
#include "xmp.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace {

constexpr uint32_t S_OK_ = 0, E_FAIL_ = 0x80004005u, E_INVALIDARG_ = 0x80070057u;
constexpr uint32_t ERROR_NO_MORE_FILES_ = 18;
constexpr uint32_t HR_NOT_FOUND = 0x800710D8u;      // 4312: the fight player finishes the custom track (121.cpp:12716-12720)

// ---- logging: the first kBudget lines of each kind; once spent a check is one relaxed load ------
constexpr uint32_t kBudget = 200;
std::atomic<uint32_t> g_ioBudget[256], g_inpBudget[256];     // by msg & 0xFF (all ids are 0x000700xx)
std::atomic<uint32_t> g_enumCreateBudget, g_enumInitBudget, g_enumReadBudget;

bool Take(std::atomic<uint32_t>& n)
{
    return n.load(std::memory_order_relaxed) < kBudget && n.fetch_add(1, std::memory_order_relaxed) < kBudget;
}

// Parameter-block dwords for the log; a bad pointer reads as zeros.
void ReadBlock(uint8_t* base, uint32_t block, uint32_t* out, uint32_t n)
{
    const bool ok = XamGuestRange(block, uint64_t(n) * 4);
    for (uint32_t i = 0; i < n; ++i) out[i] = ok ? GRead32(base, block + i * 4) : 0;
}

uint32_t Arg(uint8_t* base, uint32_t block, uint32_t offset, uint32_t length)
{
    return offset + 4 <= length ? GRead32(base, block + offset) : 0;
}

bool StubOn()
{
    static const bool on = [] {
        const char* v = std::getenv("TTT2_TUNES_STUB");
        const bool b = v && std::strcmp(v, "1") == 0;
        if (b) std::fprintf(stderr, "[xmp] stub media source on\n");
        return b;
    }();
    return on;
}

// ---- stub library: one source, three 984-byte song records ------------------------------------
// Song record as the menu reads it (plan section 1): +0..571 the identity the game saves and sends back to
// 0x7006F (host-owned), +572 title and +732 group (UTF-16BE, NUL-terminated, 80 wchar each), +892..983
// never read by the menu. The stub identity: +0 the source id below (40 bytes, also the magic), +40 u32
// version 1, +44 u32 song index, +48..571 the byte 0xA0 + index, so a save-slot dump shows it plainly.
constexpr uint32_t kRecord = 984, kIdentity = 572, kTitle = 572, kGroup = 732, kStubSongs = 3;
constexpr char kStubSourceId[] = "TTT2R-XMP-STUB";      // zero padded to 40 bytes
const char* const kStubTitles[kStubSongs] = { "Stub A", "Stub B", "Stub C" };

void PutWide(uint8_t* p, uint32_t capacity, const char* s)   // UTF-16BE from ASCII, NUL-terminated
{
    for (uint32_t i = 0; i + 1 < capacity && s[i]; ++i) { p[2 * i] = 0; p[2 * i + 1] = uint8_t(s[i]); }
}

void PutU32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
uint32_t GetU32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

std::array<uint8_t, kRecord> StubRecord(uint32_t index)
{
    std::array<uint8_t, kRecord> r{};
    std::memcpy(r.data(), kStubSourceId, sizeof(kStubSourceId));
    PutU32(r.data() + 40, 1);
    PutU32(r.data() + 44, index);
    std::memset(r.data() + 48, 0xA0 + int(index), kIdentity - 48);
    PutWide(r.data() + kTitle, 80, kStubTitles[index]);
    PutWide(r.data() + kGroup, 80, "Stub");
    return r;
}

// The stub song index behind an identity (or record) at `p`, or -1.
int StubIndex(const uint8_t* p)
{
    uint8_t id[40] = {};
    std::memcpy(id, kStubSourceId, sizeof(kStubSourceId));
    if (std::memcmp(p, id, 40) || GetU32(p + 40) != 1) return -1;
    const uint32_t index = GetU32(p + 44);
    return index < kStubSongs ? int(index) : -1;
}

// ---- stub playlist and transport (a fake state: status 1 for 30 s after Play, then 0) -----------
std::mutex g_m;
uint32_t g_playlist = 0;               // the storage pointer the game passed to 0x7000D (it XMemFree's the handle)
std::vector<int> g_playlistSongs;
std::atomic<int64_t> g_playUntilMs{0}; // steady-clock ms; 0 = stopped
std::map<uint32_t, uint32_t> g_enumOfPriv;   // enumerator private block -> handle (for the init log)

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint32_t Status() { const int64_t until = g_playUntilMs.load(); return until && NowMs() < until ? 1u : 0u; }

// XMPGetMediaSources {+0 client, +4 connected-only, +8 MSAL_MEDIASOURCEINFO* (0xB4 each), +0xC max,
// +0x10 DWORD* returned}, from sub_82852030 (168.cpp:342-388). The wrapper keeps a record only with
// type 0..4, (flags & 0x2) && !(flags & 0x200) and state 5 (168.cpp:398-560); a type-1 record whose
// state is not 5 makes it Sleep(1000) and ask again, up to 10 times (168.cpp:404-471).
uint32_t GetMediaSources(uint8_t* base, uint32_t buffer)
{
    const uint32_t sources = GRead32(base, buffer + 8), max = GRead32(base, buffer + 12), returned = GRead32(base, buffer + 16);
    if (!returned) return E_INVALIDARG_;
    if (StubOn())
    {
        if (!sources) { GWrite32(base, returned, 1); return S_OK_; }
        if (max < 1) return 0x80070008u;
        if (!XamGuestRange(sources, 0xB4)) return E_INVALIDARG_;
        std::memset(base + sources, 0, 0xB4);
        std::memcpy(base + sources, kStubSourceId, sizeof(kStubSourceId));   // +0x00: id (40 bytes)
        GWrite32(base, sources + 0x28, 0);                                    // type 0: TEKKEN TUNES uses the last type-0 source (111.cpp:2750-2764)
        GWrite32(base, sources + 0x2C, 0x2);                                  // flags: 0x2 set, 0x200 clear
        GWrite32(base, sources + 0x30, 5);                                    // state 5 = ready
        // The name field is not known (not seen in the reads checked: the filter in sub_82852030 and
        // 111.cpp:2362-2380 / 2750-2764 read only +0x00..+0x30): a guess at
        // +0x34, UTF-16BE, up to 64 wchar (the record ends at 0xB4).
        PutWide(base + sources + 0x34, 64, "Stub music");
        GWrite32(base, returned, 1);
        return S_OK_;
    }
    // A null array asks for the count (10 slots); the answer is 0 sources.
    constexpr uint32_t kSlots = 10;
    if (!sources) { GWrite32(base, returned, kSlots); return 0; }
    if (max < kSlots) return 0x80070008u;                     // E_OUTOFMEMORY-style: buffer too small
    for (uint32_t i = 0; i < kSlots; ++i)
    {
        std::memset(base + sources + i * 0xB4, 0, 0x28);
        GWrite32(base, sources + i * 0xB4, i);
    }
    GWrite32(base, returned, 0);
    return 0;
}

// 0x7006F {+0 0, +4 identity (572 bytes, unaligned: player+9), +8 out 984-byte record}, from
// sub_82852228 (168.cpp:641-690). In a fight it resolves the identity the save slot holds.
uint32_t Resolve(uint8_t* base, uint32_t buffer, uint32_t length, bool log, bool& handled)
{
    const uint32_t id = Arg(base, buffer, 4, length), out = Arg(base, buffer, 8, length);
    uint8_t identity[kIdentity] = {};
    const bool idOk = XamGuestRange(id, kIdentity, 1);
    if (idOk) for (uint32_t i = 0; i < kIdentity; ++i) identity[i] = GRead8(base, id + i);
    static std::atomic<bool> dumped{false};
    if (idOk && !dumped.exchange(true))
    {
        // The save slot (options+1604+slot*592, +8 = these bytes, 92.cpp:559-579) as the game sends it back.
        char hex[3 * 64 + 1] = {};
        for (int i = 0; i < 64; ++i) std::snprintf(hex + 3 * i, 4, "%02X ", identity[i]);
        std::fprintf(stderr, "[xmp] save slot identity at %08X (first 64 bytes): %s\n", id, hex);
    }
    uint64_t hash = 0xCBF29CE484222325ull;                   // FNV-1a over the 572 bytes
    for (uint8_t b : identity) hash = (hash ^ b) * 0x100000001B3ull;
    const int index = idOk ? StubIndex(identity) : -1;
    handled = StubOn();
    const char* result = !handled ? "unserved (stub off)" : index < 0 ? "not found (4312)" : "found";
    if (log)
        std::fprintf(stderr, "[xmp] resolve id hash=%016llX path=\"%s\" -> %s\n", (unsigned long long)hash,
                     index < 0 ? "" : kStubTitles[index], result);
    if (!handled) return E_FAIL_;
    if (index < 0) return HR_NOT_FOUND;
    if (!XamGuestRange(out, kRecord, 1)) return E_INVALIDARG_;
    const auto record = StubRecord(uint32_t(index));
    std::memcpy(base + out, record.data(), kRecord);
    return S_OK_;
}

// The stub's playlist and transport messages. Blocks (plan section 1 table, 168.cpp):
//  0x7002E {+0 client, +4 count, +8 DWORD* size}              sub_82852380 1st (839-914)
//  0x7000D {+4 storage, +8 size, +0x10 count, +0x20 HXMPPLAYLIST*} sub_82852380 2nd (1000-1030)
//  0x70011 {+4 playlist, +8 records (984 each), +0x10 count}    sub_828526C0 (1334-1410)
//  0x70013 {+0 client, +4 playlist}, then XMemFree(playlist)  sub_82852530 (1096-1196)
//  0x70002 {+0 2, +4 playlist, +8 song}                        sub_828522D8 (742-787)
//  0x70004 {+0 2, +4 0}                                        sub_82852620 (1237-1276)
uint32_t Transport(uint8_t* base, uint32_t msg, uint32_t buffer, uint32_t length, bool log, bool& handled)
{
    handled = true;
    switch (msg)
    {
    case 0x0007002E:
    {
        // Title playlist buffer size: must not be 0, or the wrapper's XMemAlloc fails.
        const uint32_t count = Arg(base, buffer, 4, length), size = Arg(base, buffer, 8, length);
        if (!XamGuestRange(size, 4)) return E_INVALIDARG_;
        GWrite32(base, size, 16 + 16 * count);
        return S_OK_;
    }
    case 0x0007000D:
    {
        const uint32_t storage = Arg(base, buffer, 4, length), size = Arg(base, buffer, 8, length);
        const uint32_t count = Arg(base, buffer, 0x10, length), out = Arg(base, buffer, 0x20, length);
        if (log) std::fprintf(stderr, "[xmp] playlist create storage=%08X size=%u count=%u\n", storage, size, count);
        if (!storage || !XamGuestRange(out, 4)) return E_INVALIDARG_;
        std::lock_guard l(g_m);
        g_playlist = storage;                 // an older playlist is dropped: the game leaks them (plan section 2)
        g_playlistSongs.clear();
        GWrite32(base, out, storage);
        return S_OK_;
    }
    case 0x00070011:
    {
        const uint32_t playlist = Arg(base, buffer, 4, length), records = Arg(base, buffer, 8, length);
        const uint32_t count = Arg(base, buffer, 0x10, length);
        if (!XamGuestRange(records, uint64_t(count) * kRecord, 1) || count > 1000) return E_INVALIDARG_;
        std::lock_guard l(g_m);
        if (playlist != g_playlist) return E_INVALIDARG_;
        const int first = count ? StubIndex(base + records) : -1;
        for (uint32_t i = 0; i < count; ++i) g_playlistSongs.push_back(StubIndex(base + records + i * kRecord));
        if (log)
            std::fprintf(stderr, "[xmp] playlist %08X add %u records (first: %s)\n", playlist, count, first >= 0 ? kStubTitles[first] : "?");
        return S_OK_;
    }
    case 0x00070013:
    {
        const uint32_t playlist = Arg(base, buffer, 4, length);
        std::lock_guard l(g_m);
        if (log) std::fprintf(stderr, "[xmp] playlist delete %08X%s\n", playlist, playlist == g_playlist ? "" : " (not the current one)");
        if (playlist == g_playlist) { g_playlist = 0; g_playlistSongs.clear(); }
        return S_OK_;
    }
    case 0x00070002:
    {
        const uint32_t playlist = Arg(base, buffer, 4, length), song = Arg(base, buffer, 8, length);
        g_playUntilMs.store(NowMs() + 30000);
        if (log)
        {
            std::lock_guard l(g_m);
            std::fprintf(stderr, "[xmp] play playlist=%08X song=%u%s\n", playlist, song, playlist == g_playlist ? "" : " (unknown playlist)");
        }
        return S_OK_;
    }
    case 0x00070004:
        g_playUntilMs.store(0);
        if (log) std::fprintf(stderr, "[xmp] stop\n");
        return S_OK_;
    default:
        handled = false;
        return E_FAIL_;
    }
}

} // namespace

uint32_t XmpStartIO(uint8_t* base, uint32_t msg, uint32_t overlapped, uint32_t buffer, uint32_t length, uint32_t lr)
{
    StubOn();                                                // its "stub on" line at the first app-0xFA message
    const bool log = Take(g_ioBudget[msg & 0xFF]);
    uint32_t blk[9] = {};
    if (log) ReadBlock(base, buffer, blk, 9);
    const uint32_t valid = XamGuestRange(buffer, length) ? length : 0;   // a bad block reads as zeros below
    bool handled = false;
    uint32_t hr = E_FAIL_;
    if (msg == 0x0007002B)
    {
        if (buffer) { handled = true; hr = GetMediaSources(base, buffer); }
    }
    else if (msg == 0x0007006F)
        hr = Resolve(base, buffer, valid, log, handled);
    else if (msg == 0x0007000C)
    {
        // XMPSetVolume {+0 2, +4 float}, sub_82852828 (168.cpp:1558-1592): the fight player sends the
        // BGM option x slot factor x a constant (121.cpp:12860-12888); the log gives the real scale.
        const uint32_t bits = Arg(base, buffer, 4, valid);
        float volume;
        std::memcpy(&volume, &bits, 4);
        if (log) std::fprintf(stderr, "[xmp] volume %f (msg 0x7000C)\n", volume);
        handled = StubOn();
        hr = handled ? S_OK_ : E_FAIL_;
    }
    else if (StubOn())
        hr = Transport(base, msg, buffer, valid, log, handled);
    if (!handled) hr = E_FAIL_;                              // as XLiveDispatch answers app 0xFA
    if (log)
        std::fprintf(stderr, "[xmp] io  msg=%08X len=%u ov=%08X lr=%08X blk=%08X %08X %08X %08X %08X %08X %08X %08X %08X -> hr=%08X\n",
                     msg, length, overlapped, lr, blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], blk[7], blk[8], hr);
    return hr;
}

uint32_t XmpInProcess(uint8_t* base, uint32_t msg, uint32_t param, uint32_t lr)
{
    StubOn();
    const bool log = Take(g_inpBudget[msg & 0xFF]);
    uint32_t blk[7] = {};
    if (log) ReadBlock(base, param, blk, 7);
    const bool block = XamGuestRange(param, 28);
    uint32_t hr = 0;
    switch (msg)
    {
    case 0x00070014:
        // Song enumerator init, sub_82851EB0 (168.cpp:118-287): {+0 client 2, +4 source id (40 bytes),
        // +8 second id, +0xC 0, +0x10 0, +0x14 flags, +0x18 priv+24}. Answered 0, as before.
        if (block && Take(g_enumInitBudget))
        {
            const uint32_t priv24 = GRead32(base, param + 0x18);
            uint32_t handle = 0;
            {
                std::lock_guard l(g_m);
                const auto it = g_enumOfPriv.find(priv24 - 24);
                if (it != g_enumOfPriv.end()) handle = it->second;
            }
            std::fprintf(stderr, "[xmp] enum init block src=%08X second=%08X flags=%08X priv24=%08X (handle %08X)\n",
                         GRead32(base, param + 4), GRead32(base, param + 8), GRead32(base, param + 0x14), priv24, handle);
        }
        break;
    case 0x00070009:
    {
        // XMPGetStatus {+0 2, +4 DWORD* state}, sub_828527C0 (168.cpp:1491-1518). The state used to be
        // left unwritten (the game read its stack); 0 = stopped unless the stub is playing.
        const uint32_t state = block ? GRead32(base, param + 4) : 0;
        const uint32_t status = StubOn() ? Status() : 0;
        if (XamGuestRange(state, 4)) GWrite32(base, state, status);
        if (log) std::fprintf(stderr, "[xmp] status -> %u\n", status);
        break;
    }
    case 0x0007001B:
        // XMPGetPlaybackController: keep the hard error (plan section 1: 0 / {0, 0} could mute the game's BGM
        // through sub_82B240C0).
        hr = 0xFFFFFFFFu;
        break;
    default:
        break;
    }
    if (log)
    {
        // out = the dword at blk[1] after the call (GetStatus: the state).
        const uint32_t out = XamGuestRange(blk[1], 4) ? GRead32(base, blk[1]) : 0;
        std::fprintf(stderr, "[xmp] inp msg=%08X lr=%08X blk=%08X %08X %08X %08X %08X %08X %08X -> hr=%08X out=%08X\n",
                     msg, lr, blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], hr, out);
    }
    return hr;
}

void XmpEnumCreated(uint32_t handle, uint32_t open, uint32_t close, uint32_t extra, uint32_t items, uint32_t priv)
{
    {
        std::lock_guard l(g_m);
        g_enumOfPriv[priv] = handle;
    }
    if (Take(g_enumCreateBudget))
        std::fprintf(stderr, "[xmp] enum create open=%08X close=%08X extra=%u items=%u handle=%08X priv=%08X\n",
                     open, close, extra, items, handle, priv);
}

void XmpEnumerate(uint8_t* base, uint32_t handle, uint32_t priv, uint32_t buffer, uint32_t bytes, bool& done,
                  uint32_t& count, uint32_t& result)
{
    (void)priv;
    count = 0;
    result = ERROR_NO_MORE_FILES_;
    // TEKKEN TUNES calls XamEnumerate once with room for 1000 records (111.cpp:2395-2464) and reads
    // the count from the overlapped's InternalHigh (111.cpp:1416).
    if (StubOn() && !done)
    {
        const uint32_t n = (std::min)(kStubSongs, bytes / kRecord);
        if (n && XamGuestRange(buffer, uint64_t(n) * kRecord, 1))
        {
            for (uint32_t i = 0; i < n; ++i)
            {
                const auto record = StubRecord(i);
                std::memcpy(base + buffer + i * kRecord, record.data(), kRecord);
            }
            count = n;
            result = 0;
        }
        done = true;
    }
    if (Take(g_enumReadBudget))
        std::fprintf(stderr, "[xmp] enum read handle=%08X bytes=%u -> %u records\n", handle, bytes, count);
}

void XmpEnumClosed(uint32_t handle)
{
    std::lock_guard l(g_m);
    for (auto it = g_enumOfPriv.begin(); it != g_enumOfPriv.end(); ++it)
        if (it->second == handle) { g_enumOfPriv.erase(it); break; }
}
