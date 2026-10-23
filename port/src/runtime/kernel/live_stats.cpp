// Xbox LIVE statistics (live_stats.h).
//
// XamUserCreateStatsEnumerator(title id, type, rank start, rows, spec count, specs, &buffer size, &handle) - the
// game's only caller is XUserCreateStatsEnumeratorByRank (type 1, sub_82BEECF0). A spec (XUSER_STATS_SPEC, 0x88)
// is {u32 view id, u32 column count, u16 column ids[64]}. XamEnumerate / XUserReadStats fill
// XUSER_STATS_READ_RESULTS:
//   results {u32 view count, u32 views}; view (0x10) {u32 id, u32 total rows, u32 rows, u32 row array};
//   row (0x30) {u64 xuid, u32 rank, pad, i64 rating, char gamertag[16], u32 column count, u32 columns};
//   column (0x18) {u16 id, pad, XUSER_DATA {u8 type, pad, 8-byte value}}.
// XSessionWriteStats (XMsg 0xB0025): {u32 session object, u32, u64 xuid, u32 view count, u32 views}; a view
// (XSESSION_VIEW_PROPERTIES, 0x0C) {u32 id, u32 property count, u32 properties}; a property (XUSER_PROPERTY, 0x18)
// {u32 id, pad, XUSER_DATA}. MEASURED (ranked match, 2026-10-22): each game writes three views for each of the two
// players - its own and the opponent's - before XSessionEnd.
// The views, their columns and how each column keeps its value are the game's own Xbox LIVE configuration
// (server/internal/stats/views.go, generated from it).
#include "live_stats.h"
#include "community_net.h"
#include "kernel_shared.h"
#include "net_transport.h"
#include "server_list.h"
#include "xlive.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace live_stats {
namespace {

constexpr uint32_t kViewBytes = 0x10, kRowBytes = 0x30, kColumnBytes = 0x18, kSpecBytes = 0x88, kPropertyBytes = 0x18;
constexpr uint8_t kTypeNull = 0xFF;
constexpr int kServerTimeoutMs = 8000;

struct Spec { uint32_t view = 0; std::vector<uint16_t> columns; };

// The server's pages for an enumerator, asked for when it was created (one per spec).
struct Pages
{
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::vector<community::StatPage> pages;
};

struct Request
{
    uint32_t title = 0, type = 0, rankStart = 0, rows = 0;
    std::vector<Spec> specs;
    std::shared_ptr<Pages> pages;
    bool done = false;
};

std::mutex g_mutex;
std::map<uint32_t, Request> g_enums;
uint32_t g_next = 0xD3000000u;

// Writes made while the saved server is not connected yet go out once it is. MEASURED (Survival, 2026-10-22): an
// offline record is written (XSessionWriteStats in a stats-only session) and then uploaded over GIN
// (POST leaderboard/), and that GIN request is what connects the saved server (gin_service.cpp) - the write comes
// first. At most 16 wait, for 2 minutes.
struct Pending
{
    std::string server;                                  // server_list::Current() when written
    uint64_t session = 0, own = 0, subject = 0;
    std::vector<community::StatViewWrite> views;
    std::chrono::steady_clock::time_point at;
};
std::mutex g_pendingMutex;
std::deque<Pending> g_pending;
bool g_flusher = false;

void Flusher()
{
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::vector<Pending> ready;
        {
            std::lock_guard l(g_pendingMutex);
            const auto now = std::chrono::steady_clock::now();
            while (!g_pending.empty() && now - g_pending.front().at > std::chrono::minutes(2))
            {
                std::fprintf(stderr, "[stats] a write for %s dropped: its server never connected\n", g_pending.front().server.c_str());
                g_pending.pop_front();
            }
            if (g_pending.empty())
            {
                g_flusher = false;
                return;
            }
            const std::shared_ptr<community::Client> server = net::Community();
            if (!server || !server->Connected()) continue;
            const std::string current = server_list::Current();
            for (auto it = g_pending.begin(); it != g_pending.end();)
            {
                if (server_list::SameAddress(it->server, current)) { ready.push_back(std::move(*it)); it = g_pending.erase(it); }
                else ++it;
            }
        }
        if (const std::shared_ptr<community::Client> server = net::Community())
            for (const Pending& w : ready)
            {
                server->WriteStats(w.session, w.own, w.subject, w.views);
                std::fprintf(stderr, "[stats] a write kept until the server connected -> the server\n");
            }
    }
}

uint64_t Read64(uint8_t* base, uint32_t a) { return (uint64_t(GRead32(base, a)) << 32) | GRead32(base, a + 4); }
void Write64(uint8_t* base, uint32_t a, uint64_t v) { GWrite32(base, a, uint32_t(v >> 32)); GWrite32(base, a + 4, uint32_t(v)); }

std::shared_ptr<community::Client> StatsServer()
{
    std::shared_ptr<community::Client> server = net::Community();
    return server && server->Connected() ? server : nullptr;
}

std::string Describe(const std::vector<Spec>& specs)
{
    std::string s;
    for (const Spec& sp : specs)
    {
        s += " view " + std::to_string(sp.view) + " [";
        for (size_t i = 0; i < sp.columns.size(); ++i) s += (i ? "," : "") + std::to_string(sp.columns[i]);
        s += "]";
    }
    return s;
}

std::vector<Spec> ReadSpecs(uint8_t* base, uint32_t specs, uint32_t count)
{
    std::vector<Spec> out;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t s = specs + i * kSpecBytes;
        Spec sp;
        sp.view = GRead32(base, s);
        const uint32_t n = (std::min)(GRead32(base, s + 4), 64u);
        for (uint32_t c = 0; c < n; ++c) sp.columns.push_back(GRead16(base, s + 8 + c * 2));
        out.push_back(std::move(sp));
    }
    return out;
}

// XUSER_STATS_READ_RESULTS for `specs` with the server's pages (a missing page = an empty view) into the title's
// buffer: false when it does not fit.
bool WriteResults(uint8_t* base, uint32_t buffer, uint32_t bytes, const std::vector<Spec>& specs,
                  const std::vector<community::StatPage>& pages)
{
    uint64_t need = 8 + uint64_t(specs.size()) * kViewBytes;
    for (size_t i = 0; i < specs.size(); ++i)
    {
        const size_t rows = i < pages.size() ? pages[i].rows.size() : 0;
        need += rows * (kRowBytes + uint64_t(specs[i].columns.size()) * kColumnBytes);
    }
    if (!buffer || need > bytes) return false;
    std::memset(base + buffer, 0, size_t(need));
    GWrite32(base, buffer, uint32_t(specs.size()));
    GWrite32(base, buffer + 4, buffer + 8);
    uint32_t next = buffer + 8 + uint32_t(specs.size()) * kViewBytes;   // rows, then columns, after the views
    for (size_t i = 0; i < specs.size(); ++i)
    {
        const Spec& sp = specs[i];
        static const community::StatPage kEmpty;
        const community::StatPage& page = i < pages.size() ? pages[i] : kEmpty;
        const uint32_t rows = uint32_t(page.rows.size());
        const uint32_t v = buffer + 8 + uint32_t(i) * kViewBytes;
        GWrite32(base, v, sp.view);
        GWrite32(base, v + 4, (std::max)(page.total, rows));
        GWrite32(base, v + 8, rows);
        GWrite32(base, v + 12, rows ? next : 0);
        const uint32_t rowArray = next;
        next += rows * kRowBytes;
        for (uint32_t k = 0; k < rows; ++k)
        {
            const community::StatRow& src = page.rows[k];
            const uint32_t row = rowArray + k * kRowBytes;
            Write64(base, row, src.xuid ? src.xuid : 0x0009000000000000ull | src.rank);
            GWrite32(base, row + 8, src.rank);
            Write64(base, row + 0x10, uint64_t(src.rating));
            std::memcpy(base + row + 0x18, src.name.c_str(), (std::min)(src.name.size(), size_t(15)));
            GWrite32(base, row + 0x28, uint32_t(sp.columns.size()));
            GWrite32(base, row + 0x2C, next);
            for (size_t c = 0; c < sp.columns.size(); ++c)
            {
                const uint32_t col = next + uint32_t(c) * kColumnBytes;
                community::StatValue value;
                for (const community::StatValue& have : src.columns)
                    if (have.column == sp.columns[c]) value = have;
                GWrite16(base, col, sp.columns[c]);
                GWrite8(base, col + 8, value.type);
                if (value.type == 1 || value.type == 5) GWrite32(base, col + 0x10, uint32_t(value.bits));
                else if (value.type != kTypeNull) Write64(base, col + 0x10, value.bits);
            }
            next += uint32_t(sp.columns.size()) * kColumnBytes;
        }
    }
    return true;
}

} // namespace

bool Enumerate(uint8_t* base, uint32_t handle, uint32_t buffer, uint32_t bytes, uint32_t& count, uint32_t& result)
{
    std::shared_ptr<Pages> pages;
    std::vector<Spec> specs;
    {
        std::lock_guard l(g_mutex);
        const auto it = g_enums.find(handle);
        if (it == g_enums.end()) return false;
        count = 0;
        result = 18;                                      // ERROR_NO_MORE_FILES
        if (it->second.done) return true;
        pages = it->second.pages;
        specs = it->second.specs;
    }
    std::vector<community::StatPage> got;
    {
        std::unique_lock l(pages->m);
        pages->cv.wait_for(l, std::chrono::milliseconds(kServerTimeoutMs), [&] { return pages->done; });
        if (pages->done) got = pages->pages;
    }
    if (!WriteResults(base, buffer, bytes, specs, got))
    {
        result = 122;                                     // ERROR_INSUFFICIENT_BUFFER
        return true;
    }
    size_t rows = 0;
    for (const community::StatPage& p : got) rows += p.rows.size();
    {
        std::lock_guard l(g_mutex);
        const auto it = g_enums.find(handle);
        if (it != g_enums.end()) it->second.done = true;
    }
    count = 1;
    result = 0;
    std::fprintf(stderr, "[stats] enumerate %08X ->%s: %zu rows\n", handle, Describe(specs).c_str(), rows);
    return true;
}

bool Close(uint32_t handle)
{
    std::lock_guard l(g_mutex);
    return g_enums.erase(handle) != 0;
}

uint32_t ReadPlayers(uint8_t* base, uint32_t b)
{
    constexpr uint32_t E_INVALIDARG = 0x80070057u, E_INSUFFICIENT_BUFFER = 0x8007007Au;
    const uint32_t nXuids = GRead32(base, b + 4), xuids = GRead32(base, b + 8);
    const uint32_t nSpecs = GRead32(base, b + 0x0C), specAddr = GRead32(base, b + 0x10);
    const uint32_t size = GRead32(base, b + 0x14), results = GRead32(base, b + 0x18);
    if (!nSpecs || nSpecs > 64 || !specAddr || !results || nXuids > community::kMaxStatRows || (nXuids && !xuids))
        return E_INVALIDARG;
    const std::vector<Spec> specs = ReadSpecs(base, specAddr, nSpecs);
    std::vector<uint64_t> players;
    for (uint32_t i = 0; i < nXuids; ++i) players.push_back(Read64(base, xuids + i * 8));
    std::vector<community::StatPage> pages(specs.size());
    if (const std::shared_ptr<community::Client> server = StatsServer())
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kServerTimeoutMs);   // all of them
        for (size_t i = 0; i < specs.size(); ++i)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) break;
            server->ReadPlayers(specs[i].view, players, specs[i].columns, pages[i], int(left));
        }
    }
    size_t rows = 0;
    for (const community::StatPage& p : pages) rows += p.rows.size();
    std::string who;
    for (uint64_t x : players)
    {
        char s[20];
        std::snprintf(s, sizeof(s), " %016llX", (unsigned long long)x);
        who += s;
    }
    if (!WriteResults(base, results, size, specs, pages))
    {
        std::fprintf(stderr, "[stats] XUserReadStats(%u players%s%s): %u bytes are too few\n", nXuids, who.c_str(), Describe(specs).c_str(), size);
        return E_INSUFFICIENT_BUFFER;
    }
    std::fprintf(stderr, "[stats] XUserReadStats(%u players%s%s): %zu rows\n", nXuids, who.c_str(), Describe(specs).c_str(), rows);
    return 0;
}

void Write(uint8_t* base, uint32_t buffer, uint32_t length, uint64_t sessionId)
{
    if (length < 0x18) return;
    const uint64_t xuid = Read64(base, buffer + 8);
    const uint32_t nViews = GRead32(base, buffer + 0x10), views = GRead32(base, buffer + 0x14);
    if (!nViews || nViews > community::kMaxStatViews || !views) return;
    std::vector<community::StatViewWrite> out;
    std::string text;
    char s[64];
    for (uint32_t i = 0; i < nViews; ++i)
    {
        const uint32_t v = views + i * 12;
        community::StatViewWrite w;
        w.view = GRead32(base, v);
        const uint32_t n = GRead32(base, v + 4), props = GRead32(base, v + 8);
        if (n > community::kMaxStatProps || (n && !props)) return;
        std::snprintf(s, sizeof(s), " view %u {", w.view);
        text += s;
        for (uint32_t k = 0; k < n; ++k)
        {
            const uint32_t p = props + k * kPropertyBytes;
            community::StatProperty prop;
            prop.id = GRead32(base, p);
            prop.type = GRead8(base, p + 8);
            switch (prop.type)
            {
            case 1: prop.bits = uint64_t(int64_t(int32_t(GRead32(base, p + 0x10)))); break;   // int32
            case 5: prop.bits = GRead32(base, p + 0x10); break;                                 // float
            case 2: case 3: case 7: prop.bits = Read64(base, p + 0x10); break;                  // int64, double, time
            default: continue;                                                                  // text / binary: no statistic
            }
            if (prop.type == 1) std::snprintf(s, sizeof(s), "%s%08X=%d", k ? " " : "", prop.id, int32_t(prop.bits));
            else std::snprintf(s, sizeof(s), "%s%08X=%lld/t%u", k ? " " : "", prop.id, (long long)prop.bits, prop.type);
            text += s;
            w.props.push_back(prop);
        }
        text += "}";
        out.push_back(std::move(w));
    }
    const std::shared_ptr<community::Client> server = StatsServer();
    const std::string saved = server ? std::string() : server_list::Current();
    std::fprintf(stderr, "[stats] XSessionWriteStats session %016llX about %016llX:%s%s\n", (unsigned long long)sessionId,
                 (unsigned long long)xuid, text.c_str(), server ? " -> the server" : saved.empty() ? "" : " (kept until the server connects)");
    if (server) server->WriteStats(sessionId, xlive::OnlineXuid(), xuid, out);
    else if (!saved.empty())
    {
        std::lock_guard l(g_pendingMutex);
        if (g_pending.size() >= 16) g_pending.pop_front();
        g_pending.push_back({ saved, sessionId, xlive::OnlineXuid(), xuid, std::move(out), std::chrono::steady_clock::now() });
        if (!g_flusher)
        {
            g_flusher = true;
            std::thread(Flusher).detach();
        }
    }
}

} // namespace live_stats

// r3 title id, r4 type (1 = by rank), r5 rank start, r6 rows, r7 spec count, r8 specs, r9 &buffer size, r10 &handle.
PPC_FUNC(__imp__XamUserCreateStatsEnumerator)
{
    using namespace live_stats;
    Request r;
    r.title = ctx.r3.u32;
    r.type = ctx.r4.u32;
    r.rankStart = ctx.r5.u32;
    r.rows = ctx.r6.u32;
    const uint32_t nSpecs = ctx.r7.u32, specs = ctx.r8.u32, sizeOut = ctx.r9.u32, handleOut = ctx.r10.u32;
    if (!nSpecs || nSpecs > 64 || !specs || !handleOut || r.rows > 1000)
    {
        std::fprintf(stderr, "[stats] XamUserCreateStatsEnumerator(type %u, specs %u, rows %u): invalid\n", r.type, nSpecs, r.rows);
        ctx.r3.u64 = 87;                                  // ERROR_INVALID_PARAMETER
        return;
    }
    r.specs = ReadSpecs(base, specs, nSpecs);
    uint32_t columns = 0;
    for (const Spec& sp : r.specs) columns += uint32_t(sp.columns.size());
    const uint32_t rows = (std::min)(r.rows, uint32_t(community::kMaxStatRows));
    const uint32_t size = 8 + nSpecs * kViewBytes + rows * nSpecs * kRowBytes + rows * columns * kColumnBytes;
    if (sizeOut) GWrite32(base, sizeOut, size);
    // Ask the server now: the page is usually there by the time the game enumerates.
    r.pages = std::make_shared<Pages>();
    const std::shared_ptr<community::Client> server = StatsServer();
    if (server && r.type == 1)
    {
        std::thread([server, pages = r.pages, specs = r.specs, start = r.rankStart, rows] {
            std::vector<community::StatPage> got(specs.size());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kServerTimeoutMs);
            for (size_t i = 0; i < specs.size(); ++i)
            {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0) break;
                server->ReadBoard(specs[i].view, start, rows, 0, specs[i].columns, got[i], int(left));
            }
            std::lock_guard l(pages->m);
            pages->pages = std::move(got);
            pages->done = true;
            pages->cv.notify_all();
        }).detach();
    }
    else
        r.pages->done = true;                             // no server: every board is empty
    uint32_t handle;
    {
        std::lock_guard l(g_mutex);
        handle = g_next++;
        g_enums.emplace(handle, r);
    }
    GWrite32(base, handleOut, handle);
    std::fprintf(stderr, "[stats] XamUserCreateStatsEnumerator(title %08X, type %u, from rank %u, %u rows):%s -> %08X, %u bytes%s\n",
                 r.title, r.type, r.rankStart, r.rows, Describe(r.specs).c_str(), handle, size, server ? " (the server)" : "");
    ctx.r3.u64 = 0;
}
