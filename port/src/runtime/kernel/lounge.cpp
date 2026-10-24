// ONLINE MODE -> FIGHTING LOUNGE (lounge.h).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "lounge.h"
#include "menu_host.h"
#include "net_transport.h"
#include "profiles.h"
#include "ranked_match.h"
#include "sys_ui.h"
#include "toast.h"
#include "xlive.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace lounge {
namespace {

constexpr uint8_t kVersion = 1;
constexpr uint32_t kLoungeContext = 0x7E5E0010u;          // a session record's context: "this is a lounge" (port's id)
constexpr int kCabinets = 20, kMaxMembers = 32, kRows = 8;  // kRows: menu_doc.cpp kLoungeRows
constexpr size_t kChatLength = 80, kChatKept = 20;
constexpr int64_t kPingMs = 2000, kMemberTimeoutMs = 10000, kHostTimeoutMs = 12000, kSnapshotMs = 3000;
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kA = 0x1000, kB = 0x2000, kX = 0x4000;
constexpr uint32_t kHintX = 57383;                         // the X key's help number (server_ui.cpp)

const char* const kFrame = "FrameOnlineLounge";
const char* const kMenu = "MenuOnlineLounge";
const char* const kOnlineMenu = "FrameOnlineMenu";
const char* const kOnlineMenuItems = "MenuOnlineMenu";
const char* const kPlayerMatchFrame = "FrameOnline0201-quick_match";   // PLAYER MATCH's page: QUICK MATCH first
const char* const kPlayerMatchMenu = "MenuOnline0201-quick_match";     // (a guessed name reads -1: taken as row 0)
constexpr int kPlayerMatchPlace = 1;                       // PLAYER MATCH's row of MenuOnlineMenu

enum Msg : uint8_t { Hello = 1, Snapshot = 2, Chat = 3, ChatOut = 4, Act = 5, Bye = 6, Start = 7, Result = 8, Closed = 9, Ping = 10 };
enum Action : uint8_t { Sit = 1, Leave = 2, Ready = 3 };
enum CabinetState : uint8_t { Open = 0, Playing = 1 };

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Text from another game, made safe for the game's text system (printable ASCII without '~' and '%').
std::string Clean(const std::string& s, size_t maxLen)
{
    std::string out;
    for (unsigned char c : s)
        if (c >= 0x20 && c < 0x7F && c != '~' && c != '%') out.push_back(char(c));
    if (out.size() > maxLen) out.resize(maxLen);
    return out;
}
std::string Upper(std::string s)
{
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
// 01..20: unique ids (the concept's four banks of five: 01-05, 06-10, 11-15, 16-20)
std::string CabinetName(int i) { return std::string(i + 1 < 10 ? "0" : "") + std::to_string(i + 1); }

// ---- messages
struct Writer
{
    std::vector<uint8_t> b;
    explicit Writer(Msg m) { b = { kVersion, uint8_t(m) }; }
    void U8(uint8_t v) { b.push_back(v); }
    void U32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void U64(uint64_t v) { U32(uint32_t(v)); U32(uint32_t(v >> 32)); }
    void Str(const std::string& s) { const size_t n = (std::min)(s.size(), size_t(255)); U8(uint8_t(n)); b.insert(b.end(), s.begin(), s.begin() + n); }
};
struct Reader
{
    const uint8_t* p;
    size_t n, at = 0;
    bool ok = true;
    Reader(const uint8_t* data, size_t size) : p(data), n(size) {}
    uint8_t U8() { if (at + 1 > n) { ok = false; return 0; } return p[at++]; }
    uint32_t U32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(U8()) << (8 * i); return v; }
    uint64_t U64() { const uint64_t lo = U32(); return lo | uint64_t(U32()) << 32; }
    std::string Str()
    {
        const size_t len = U8();
        if (!ok || at + len > n) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p + at), len);
        at += len;
        return s;
    }
};

// ---- state
struct Cabinet
{
    uint64_t seat[2] = {};
    bool ready[2] = {};
    std::vector<uint64_t> queue;
    uint8_t state = Open;
    uint32_t match = 0;
    std::string last;                                     // the last match's result, as the page shows it
    // the host's view of the match being played: the reports so far
    uint64_t winner = 0;
    int reports = 0;
    int64_t firstReportMs = 0;
};
struct Member { uint64_t peer = 0; std::string name; int64_t heardMs = 0; };
struct ChatLine { std::string name, text; };
enum class Phase { Off, Searching, Joining, In };
// The walk from the lounge to a cabinet's match and back (main thread).
enum class Walk { None, LeavePage, ToPlayerMatch, ToQuickMatch, Searching, InMatch, Back, ToLounge };

struct State
{
    Phase phase = Phase::Off;
    bool host = false;
    uint64_t hostPeer = 0, recordId = 0;
    int64_t hostHeardMs = 0, joinUntilMs = 0;
    std::vector<uint64_t> tried;                          // lounge hosts that did not answer this search
    std::vector<Member> members;
    Cabinet cab[kCabinets];
    std::deque<ChatLine> chat;
    uint32_t chatSerial = 0;
    std::string closedWhy;
    // this game's cabinet match: told by the host (Start), walked to on the main thread
    bool startPending = false;
    int matchCab = -1;
    uint32_t match = 0;
    uint64_t opponent = 0;
    int winnerSide = -1;
    bool overPending = false;
    bool overEnded = false;
} g;
std::mutex g_m;
std::atomic<bool> g_searchRunning{ false };

uint64_t Self() { return xlive::LocalPeerId(); }

struct Out { uint64_t to; std::vector<uint8_t> bytes; };
void SendAll(const std::vector<Out>& out)
{
    for (const Out& o : out) XSocketSendLounge(o.to, o.bytes.data(), o.bytes.size());
}

const Member* MemberOf(uint64_t peer)
{
    for (const Member& m : g.members)
        if (m.peer == peer) return &m;
    return nullptr;
}
std::string NameOf(uint64_t peer)
{
    if (const Member* m = MemberOf(peer)) return m->name;
    return "?";
}
int SeatOf(const Cabinet& c, uint64_t peer)
{
    for (int s = 0; s < 2; ++s)
        if (c.seat[s] == peer) return s;
    return -1;
}

// ---- the host (under g_m): the whole state, and its handling of requests
Writer SnapshotLocked()
{
    Writer w(Snapshot);
    w.U8(uint8_t(g.members.size()));
    for (const Member& m : g.members) { w.U64(m.peer); w.Str(m.name); }
    for (const Cabinet& c : g.cab)
    {
        w.U64(c.seat[0]); w.U64(c.seat[1]);
        w.U8(uint8_t(c.ready[0] | c.ready[1] << 1));
        w.U8(c.state);
        w.U32(c.match);
        w.Str(c.last);
        w.U8(uint8_t(c.queue.size()));
        for (uint64_t q : c.queue) w.U64(q);
    }
    return w;
}
void BroadcastLocked(const Writer& w, std::vector<Out>& out)
{
    for (const Member& m : g.members)
        if (m.peer != Self()) out.push_back({ m.peer, w.b });
}
// Removes a player from every seat and queue (a seat of a match being played stays).
void UnseatLocked(uint64_t peer)
{
    for (Cabinet& c : g.cab)
    {
        std::erase(c.queue, peer);
        if (c.state == Playing) continue;
        for (int s = 0; s < 2; ++s)
            if (c.seat[s] == peer) { c.seat[s] = 0; c.ready[0] = c.ready[1] = false; }
    }
}
// Fills a free seat from the front of the queue.
void FillLocked(Cabinet& c)
{
    for (int s = 0; s < 2 && !c.queue.empty(); ++s)
        if (!c.seat[s]) { c.seat[s] = c.queue.front(); c.queue.erase(c.queue.begin()); c.ready[0] = c.ready[1] = false; }
}
void ChatLocked(const std::string& name, const std::string& text, std::vector<Out>& out);

void StartLocked(int i, std::vector<Out>& out)
{
    Cabinet& c = g.cab[i];
    c.state = Playing;
    ++c.match;
    c.winner = 0;
    c.reports = 0;
    c.firstReportMs = 0;
    std::fprintf(stderr, "[lounge] cabinet %s: match %u, %016llX vs %016llX\n", CabinetName(i).c_str(), c.match,
                 (unsigned long long)c.seat[0], (unsigned long long)c.seat[1]);
    for (int s = 0; s < 2; ++s)
    {
        Writer w(Start);
        w.U8(uint8_t(i));
        w.U32(c.match);
        w.U64(c.seat[1 - s]);
        if (c.seat[s] == Self())
        {
            g.startPending = true;
            g.matchCab = i;
            g.match = c.match;
            g.opponent = c.seat[1 - s];
        }
        else out.push_back({ c.seat[s], w.b });
    }
}

// The match at cabinet i is decided (winner 0: not known): the winner stays, the loser goes to the back of the queue
// and the next challenger sits down.
void FinishLocked(int i, std::vector<Out>& out)
{
    Cabinet& c = g.cab[i];
    c.state = Open;
    c.ready[0] = c.ready[1] = false;
    const int winSeat = c.winner ? SeatOf(c, c.winner) : -1;
    c.last = winSeat >= 0 ? Upper(NameOf(c.winner)) + " WON" : "MATCH OVER";
    std::fprintf(stderr, "[lounge] cabinet %s: match %u over, %s\n", CabinetName(i).c_str(), c.match, c.last.c_str());
    if (!c.queue.empty())
    {
        const int loseSeat = winSeat >= 0 ? 1 - winSeat : 1;     // no winner known: seat 2 rotates
        const uint64_t loser = c.seat[loseSeat];
        c.seat[loseSeat] = 0;
        if (loser) c.queue.push_back(loser);
        FillLocked(c);
    }
    (void)out;
}

void HostHandleLocked(uint64_t from, Msg type, Reader& r, std::vector<Out>& out)
{
    for (Member& m : g.members)
        if (m.peer == from) m.heardMs = NowMs();
    bool changed = false;
    switch (type)
    {
    case Hello:
    {
        const std::string name = Clean(r.Str(), profiles::kMaxName);
        if (!r.ok) return;
        if (!MemberOf(from))
        {
            if (int(g.members.size()) >= kMaxMembers)
            {
                Writer w(Closed);
                w.Str("The lounge is full.");
                out.push_back({ from, w.b });
                return;
            }
            g.members.push_back({ from, name.empty() ? std::string("PLAYER") : name, NowMs() });
            std::fprintf(stderr, "[lounge] %s (%016llX) joined, %zu here\n", name.c_str(), (unsigned long long)from, g.members.size());
            ChatLocked("LOUNGE", name + " joined.", out);
        }
        changed = true;
        break;
    }
    case Ping:
        return;
    case Bye:
    {
        const Member* m = MemberOf(from);
        if (!m) return;
        const std::string name = m->name;
        UnseatLocked(from);
        std::erase_if(g.members, [&](const Member& x) { return x.peer == from; });
        for (Cabinet& c : g.cab) FillLocked(c);
        ChatLocked("LOUNGE", name + " left.", out);
        changed = true;
        break;
    }
    case Chat:
    {
        const std::string text = Clean(r.Str(), kChatLength);
        if (!r.ok || text.empty() || !MemberOf(from)) return;
        ChatLocked(NameOf(from), text, out);
        return;
    }
    case Act:
    {
        const int i = r.U8();
        const uint8_t action = r.U8();
        if (!r.ok || i >= kCabinets || !MemberOf(from)) return;
        Cabinet& c = g.cab[i];
        const int seat = SeatOf(c, from);
        if (action == Sit && seat < 0 && std::find(c.queue.begin(), c.queue.end(), from) == c.queue.end())
        {
            UnseatLocked(from);                       // one cabinet at a time
            if (c.state == Open && !c.seat[0]) c.seat[0] = from;
            else if (c.state == Open && !c.seat[1]) c.seat[1] = from;
            else c.queue.push_back(from);
            c.ready[0] = c.ready[1] = false;
            changed = true;
        }
        else if (action == Leave)
        {
            UnseatLocked(from);
            FillLocked(c);
            changed = true;
        }
        else if (action == Ready && seat >= 0 && c.state == Open && c.seat[0] && c.seat[1])
        {
            c.ready[seat] = !c.ready[seat];
            if (c.ready[0] && c.ready[1]) StartLocked(i, out);
            changed = true;
        }
        break;
    }
    case Result:
    {
        const int i = r.U8();
        const uint32_t match = r.U32();
        const uint64_t winner = r.U64();
        if (!r.ok || i >= kCabinets) return;
        Cabinet& c = g.cab[i];
        if (c.state != Playing || c.match != match || SeatOf(c, from) < 0) return;   // late, duplicate or not a player
        if (!c.winner && SeatOf(c, winner) >= 0) c.winner = winner;
        if (++c.reports == 1) c.firstReportMs = NowMs();
        if (c.reports >= 2 || c.winner) FinishLocked(i, out);
        changed = true;
        break;
    }
    default:
        return;
    }
    if (changed) BroadcastLocked(SnapshotLocked(), out);
}

// ---- every member (under g_m)
void AddChatLocked(const std::string& name, const std::string& text)
{
    g.chat.push_back({ name, text });
    while (g.chat.size() > kChatKept) g.chat.pop_front();
    ++g.chatSerial;
    toast::Note note;
    note.title = Upper(name);
    note.lines = { text.size() > 40 ? text.substr(0, 40) : text };
    if (text.size() > 40) note.lines.push_back(text.substr(40));
    toast::Show(note, 6000);
}
void ChatLocked(const std::string& name, const std::string& text, std::vector<Out>& out)
{
    AddChatLocked(name, text);
    Writer w(ChatOut);
    w.Str(name);
    w.Str(text);
    BroadcastLocked(w, out);
}

void ApplySnapshotLocked(Reader& r)
{
    std::vector<Member> members(r.U8());
    for (Member& m : members) { m.peer = r.U64(); m.name = Clean(r.Str(), profiles::kMaxName); }
    Cabinet cab[kCabinets];
    for (Cabinet& c : cab)
    {
        c.seat[0] = r.U64(); c.seat[1] = r.U64();
        const uint8_t ready = r.U8();
        c.ready[0] = ready & 1; c.ready[1] = (ready >> 1) & 1;
        c.state = r.U8();
        c.match = r.U32();
        c.last = Clean(r.Str(), 40);
        c.queue.resize(r.U8());
        for (uint64_t& q : c.queue) q = r.U64();
    }
    if (!r.ok) return;
    g.members = std::move(members);
    for (int i = 0; i < kCabinets; ++i) g.cab[i] = std::move(cab[i]);
}

// A request to the host: handled here when this game hosts, else sent.
void Request(const Writer& w)
{
    std::vector<Out> out;
    {
        std::lock_guard l(g_m);
        if (g.phase != Phase::In) return;
        if (g.host)
        {
            Reader r(w.b.data() + 2, w.b.size() - 2);
            HostHandleLocked(Self(), Msg(w.b[1]), r, out);
        }
        else out.push_back({ g.hostPeer, w.b });
    }
    SendAll(out);
}

// ---- joining and leaving
std::vector<uint8_t> HelloBytes()
{
    Writer w(Hello);
    w.Str(Clean(profiles::Active().name, profiles::kMaxName));
    return w.b;
}

void PublishLocked()
{
    mm::SessionRecord r;
    r.id = g.recordId;
    r.hostPeer = Self();
    r.hostXuid = xlive::OnlineXuid();
    r.maxPublic = kMaxMembers;
    r.filledPublic = uint32_t(g.members.size());
    r.contexts.push_back({ kLoungeContext, kVersion });
    std::thread([r] { mm::Publish(r); }).detach();
}

void HostLocked()
{
    g.phase = Phase::In;
    g.host = true;
    g.hostPeer = Self();
    std::mt19937_64 rng{ std::random_device{}() };
    g.recordId = rng() | 1;
    g.members = { { Self(), Clean(profiles::Active().name, profiles::kMaxName), NowMs() } };
    for (Cabinet& c : g.cab) c = Cabinet{};
    PublishLocked();
    std::fprintf(stderr, "[lounge] hosting lounge %016llX\n", (unsigned long long)g.recordId);
}

// Looks for a lounge to join (a worker thread: listing may wait on Steam); none answering - this game hosts one.
void Search()
{
    if (g_searchRunning.exchange(true)) return;
    std::thread([] {
        XSocketStartTransport();
        std::vector<mm::SessionRecord> found;
        for (mm::SessionRecord& r : mm::List(32))
            if (IsLoungeRecord(r) && r.hostPeer != Self()) found.push_back(std::move(r));
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.filledPublic > b.filledPublic; });
        std::vector<uint8_t> hello;
        uint64_t host = 0;
        {
            std::lock_guard l(g_m);
            if (g.phase == Phase::Searching)
            {
                for (const mm::SessionRecord& r : found)
                    if (std::find(g.tried.begin(), g.tried.end(), r.hostPeer) == g.tried.end()) { host = r.hostPeer; break; }
                if (host)
                {
                    g.phase = Phase::Joining;
                    g.hostPeer = host;
                    g.joinUntilMs = NowMs() + 5000;
                    g.tried.push_back(host);
                    hello = HelloBytes();
                    std::fprintf(stderr, "[lounge] joining %016llX's lounge\n", (unsigned long long)host);
                }
                else HostLocked();
            }
        }
        if (host) XSocketSendLounge(host, hello.data(), hello.size());
        g_searchRunning = false;
    }).detach();
}

void Enter()
{
    {
        std::lock_guard l(g_m);
        if (g.phase != Phase::Off) return;
        g.phase = Phase::Searching;
        g.host = false;
        g.tried.clear();
        g.closedWhy.clear();
        g.members.clear();
        for (Cabinet& c : g.cab) c = Cabinet{};
    }
    std::fprintf(stderr, "[lounge] entering\n");
    Search();
}

void LeaveLounge()
{
    std::vector<Out> out;
    uint64_t record = 0;
    {
        std::lock_guard l(g_m);
        if (g.phase == Phase::Off) return;
        if (g.host)
        {
            Writer w(Closed);
            w.Str("The lounge's host left.");
            BroadcastLocked(w, out);
            record = g.recordId;
        }
        else if (g.phase == Phase::In || g.phase == Phase::Joining)
            out.push_back({ g.hostPeer, Writer(Bye).b });
        g.phase = Phase::Off;
        g.host = false;
        g.members.clear();
        g.startPending = g.overPending = false;
    }
    std::fprintf(stderr, "[lounge] left\n");
    SendAll(out);
    if (record) std::thread([record] { mm::Remove(record); }).detach();
}

// ---- the page (main thread)
struct RowText { std::string label, value, help; bool visible = false; };
struct Ui
{
    std::string frame, candidate;
    int candidateFrames = 0;
    bool pageUp = false, drawn = false;
    int settle = 0, top = 0;
    RowText shown[kRows];
    uint16_t prev = 0, repeatKey = 0;
    int64_t repeatAt = 0;
    std::atomic<uint16_t> mask{ 0xFFFF };
    bool typing = false;
    // the page's menu stays inactive after a window of ours closed over it (it waits for an answer of its own
    // window - MEASURED 2026-10-24: no cursor moves after LOUNGE CHAT): shown again, it takes input again
    std::atomic<bool> reshow{ false };
    // the walk to a match and back
    Walk walk = Walk::None;
    int64_t nextPulseMs = 0, walkUntilMs = 0, frameSinceMs = 0;
    int64_t nextPingMs = 0, nextSnapshotMs = 0, nextPublishMs = 0;
} ui;

std::string Two(int i) { return std::string(i < 10 ? "0" : "") + std::to_string(i); }

uint16_t Pressed()
{
    const uint16_t now = HostControllerButtons();
    uint16_t pressed = now & ~ui.prev;
    const uint16_t dirs = now & (kUp | kDown);
    const int64_t t = NowMs();
    if (dirs && dirs == ui.repeatKey && t >= ui.repeatAt) { pressed |= dirs; ui.repeatAt = t + 90; }
    else if (dirs != ui.repeatKey) { ui.repeatKey = dirs; ui.repeatAt = t + 400; }
    ui.prev = now;
    return pressed;
}

constexpr int kEntries = 1 + kCabinets;                   // LOUNGE CHAT, then the cabinets

RowText TextOf(int entry)
{
    RowText r;
    r.visible = true;
    std::lock_guard l(g_m);
    if (entry == 0)
    {
        r.label = "LOUNGE CHAT";
        switch (g.phase)
        {
        case Phase::Off: r.value = "CLOSED"; break;
        case Phase::Searching:
        case Phase::Joining: r.value = "SEARCHING..."; break;
        case Phase::In:
            r.value = std::to_string(g.members.size()) + (g.members.size() == 1 ? " PLAYER" : " PLAYERS") + (g.host ? " (HOST)" : "");
            break;
        }
        if (g.phase == Phase::Off && !g.closedWhy.empty()) r.help = g.closedWhy + " A: open the lounge again.";
        else if (!g.chat.empty()) r.help = Clean(g.chat.back().name + ": " + g.chat.back().text, 44);
        else r.help = "A: say something to everyone in the lounge.";
        return r;
    }
    const int i = entry - 1;
    const Cabinet& c = g.cab[i];
    r.label = "CABINET " + CabinetName(i);
    const int seated = int(c.seat[0] != 0) + int(c.seat[1] != 0);
    const int mine = SeatOf(c, Self());
    const bool queued = std::find(c.queue.begin(), c.queue.end(), Self()) != c.queue.end();
    if (c.state == Playing) r.value = "PLAYING";
    else if (seated == 0) r.value = "OPEN";
    else if (seated == 1) r.value = "WAITING 1/2";
    else r.value = "READY " + std::to_string(int(c.ready[0]) + int(c.ready[1])) + "/2";
    if (!c.queue.empty()) r.value += "  +" + std::to_string(c.queue.size());
    std::string help;
    if (seated)
        help = Upper(c.seat[0] ? NameOf(c.seat[0]) : std::string("-")) + " VS " + Upper(c.seat[1] ? NameOf(c.seat[1]) : std::string("-")) + ". ";
    if (mine >= 0 && c.state == Open)
        help += seated == 2 ? (c.ready[mine] ? "Ready! X: leave." : "A: ready  X: leave.") : "Waiting for a challenger. X: leave.";
    else if (queued)
    {
        const size_t at = size_t(std::find(c.queue.begin(), c.queue.end(), Self()) - c.queue.begin());
        help += "You are #" + std::to_string(at + 1) + " in line. X: leave.";
    }
    else if (mine < 0)
        help += c.state == Open && seated < 2 ? "A: sit down." : "A: join the queue.";
    if (!c.last.empty() && help.size() < 30) help += " Last: " + c.last + ".";
    r.help = Clean(help, 60);
    return r;
}

void DrawPage(PPCContext& ctx, uint8_t* base, bool force)
{
    for (int k = 0; k < kRows; ++k)
    {
        const int at = ui.top + k;
        const RowText want = at < kEntries ? TextOf(at) : RowText{};
        RowText& was = ui.shown[k];
        const std::string row = "MenuItemOnlineLng" + Two(k + 1);
        if (force || want.visible != was.visible)
        {
            menu_host::SetVisible(ctx, base, row, want.visible);
            menu_host::SetFocusable(ctx, base, row, want.visible);
            menu_host::SetVisible(ctx, base, "SelectValueOnlineLng" + Two(k + 1), want.visible);
        }
        if (force || want.label != was.label) menu_host::SetText(ctx, base, row, want.label.empty() ? " " : want.label);
        if (force || want.value != was.value) menu_host::SetText(ctx, base, "SelectValueOnlineLng" + Two(k + 1), want.value);
        if (force || want.help != was.help) menu_host::SetText(ctx, base, "StaticOnlineLng00" + std::to_string(k + 1), want.help);
        was = want;
    }
    menu_host::MovieVisible(ctx, base, "yajirushi_up", ui.top > 0);
    menu_host::MovieVisible(ctx, base, "yajirushi_down", ui.top + kRows < kEntries);
    ui.drawn = true;
}

void TypeChat()
{
    SysUiKeyboard kb;
    kb.title = "LOUNGE CHAT";
    kb.description = "Everyone in the lounge sees it.";
    kb.maxLength = kChatLength;
    ui.typing = true;
    kb.done = [](bool ok, const std::string& text) {
        ui.typing = false;
        ui.reshow = true;
        const std::string clean = Clean(text, kChatLength);
        if (!ok || clean.empty()) return;
        Writer w(Chat);
        w.Str(clean);
        Request(w);
    };
    if (!SysUiShowKeyboard(std::move(kb))) ui.typing = false;
}

void PageFrame(PPCContext& ctx, uint8_t* base)
{
    if (ui.settle > 0) { --ui.settle; Pressed(); return; }
    if (ui.reshow.exchange(false))
    {
        menu_host::ShowFrame(ctx, base, kFrame);
        ui.settle = 2;
        ui.drawn = false;
        return;
    }
    const bool force = !ui.drawn;
    const uint16_t pressed = Pressed();
    const int c = menu_host::Cursor(ctx, base, kMenu);
    const bool idle = !ui.typing && !SysUiBoxBusy() && !menu_host::WindowShowing(ctx, base) && ui.walk == Walk::None;
    if (idle)
    {
        if ((pressed & kDown) && c == kRows - 1 && ui.top + kRows < kEntries) { ++ui.top; menu_host::PlaySound(ctx, base, menu_host::SoundCursor); }
        else if ((pressed & kUp) && c == 0 && ui.top > 0) { --ui.top; menu_host::PlaySound(ctx, base, menu_host::SoundCursor); }
        const int entry = c >= 0 && c < kRows ? ui.top + c : -1;
        Phase phase;
        {
            std::lock_guard l(g_m);
            phase = g.phase;
        }
        if (pressed & kB)
        {
            // the page's own B does nothing (MEASURED 2026-10-24): back to the ONLINE MODE menu from here
            menu_host::PlaySound(ctx, base, menu_host::SoundCancel);
            menu_host::ShowFrame(ctx, base, kOnlineMenu);
        }
        else if (entry == 0 && (pressed & kA))
        {
            menu_host::PlaySound(ctx, base, menu_host::SoundDecide);
            if (phase == Phase::In) TypeChat();
            else if (phase == Phase::Off) Enter();
        }
        else if (entry >= 1 && entry <= kCabinets && (pressed & (kA | kX)) && phase == Phase::In)
        {
            menu_host::PlaySound(ctx, base, menu_host::SoundDecide);
            const int i = entry - 1;
            uint8_t action;
            {
                std::lock_guard l(g_m);
                const Cabinet& cab = g.cab[i];
                const int mine = SeatOf(cab, Self());
                if (pressed & kX) action = Leave;
                else if (mine >= 0) action = Ready;
                else action = Sit;
            }
            Writer w(Act);
            w.U8(uint8_t(i));
            w.U8(action);
            Request(w);
        }
    }
    DrawPage(ctx, base, force);
    // on the way to a match the page leaves the buttons alone: the walk's pulses only reach an unmasked game
    ui.mask = ui.walk != Walk::None ? uint16_t(0xFFFF) : uint16_t(0xFFFF & ~kA & ~kB & ~kX & ~(c == kRows - 1 ? kDown : 0) & ~(c == 0 ? kUp : 0));
}

// The walk to a cabinet's match (unranked): off the page, PLAYER MATCH on the ONLINE MODE menu (UP / DOWN until
// there, A), then QUICK MATCH (A); its search finds the cabinet opponent alone (ranked::PlanLoungeMatch). After the
// match: back to the ONLINE MODE menu (B, a question's first answer A) and into the lounge.
void WalkFrame(PPCContext& ctx, uint8_t* base, const std::string& frame)
{
    const int64_t t = NowMs();
    if (ui.walk != Walk::None && ui.walk != Walk::InMatch && t > ui.walkUntilMs)
    {
        std::fprintf(stderr, "[lounge] walk %d gave up on frame %s\n", int(ui.walk), frame.c_str());
        ui.walk = Walk::None;
    }
    if (t < ui.nextPulseMs) return;
    switch (ui.walk)
    {
    case Walk::LeavePage:
        if (frame == kFrame) { menu_host::ShowFrame(ctx, base, kOnlineMenu); ui.nextPulseMs = t + 1500; }
        else if (frame == kOnlineMenu) { ui.walk = Walk::ToPlayerMatch; ui.nextPulseMs = t + 400; }
        break;
    case Walk::ToPlayerMatch:
    {
        // a menu just shown ignores presses for a moment (MEASURED 2026-10-24: an A 0.4 s after the switch was lost)
        if (frame != kOnlineMenu || t - ui.frameSinceMs < 1500) break;
        const int at = menu_host::Cursor(ctx, base, kOnlineMenuItems);
        if (at >= 0 && at != kPlayerMatchPlace) { HostPulseButtons(at > kPlayerMatchPlace ? kUp : kDown); ui.nextPulseMs = t + 250; }
        else
        {
            HostPulseButtons(kA);
            ui.walk = Walk::ToQuickMatch;
            ui.nextPulseMs = t + 1500;
            std::fprintf(stderr, "[lounge] PLAYER MATCH chosen\n");
        }
        break;
    }
    case Walk::ToQuickMatch:
        // a tip window on the way (A), then QUICK MATCH (A)
        if (menu_host::WindowShowing(ctx, base)) { HostPulseButtons(kA); ui.nextPulseMs = t + 1200; }
        else if (frame == kPlayerMatchFrame && t - ui.frameSinceMs >= 1200)
        {
            if (menu_host::Cursor(ctx, base, kPlayerMatchMenu) > 0) { HostPulseButtons(kUp); ui.nextPulseMs = t + 250; }
            else
            {
                HostPulseButtons(kA);
                ui.walk = Walk::Searching;
                ui.walkUntilMs = t + 20000;
                ui.nextPulseMs = t + 1500;
                std::fprintf(stderr, "[lounge] QUICK MATCH chosen\n");
                toast::Note n;
                n.title = "FIGHTING LOUNGE";
                n.lines = { "Finding your cabinet opponent...", "Unranked: your rank does not change." };
                toast::Show(n, 6000);
            }
        }
        else if (frame == kOnlineMenu && t - ui.frameSinceMs >= 3000) ui.walk = Walk::ToPlayerMatch;   // the A was lost
        break;
    case Walk::Searching:
        // QUICK MATCH's questions while it searches (the host's "create a session?") take their first answer
        if (frame == kPlayerMatchFrame && menu_host::WindowShowing(ctx, base)) { HostPulseButtons(kA); ui.nextPulseMs = t + 1200; }
        else if (frame != kPlayerMatchFrame && t - ui.frameSinceMs >= 1500) ui.walk = Walk::InMatch;   // in the room
        break;
    case Walk::Back:
        if (frame == kOnlineMenu) { ui.walk = Walk::ToLounge; ui.nextPulseMs = t + 400; }
        else if (!menu_host::WindowShowing(ctx, base)) { HostPulseButtons(kB); ui.nextPulseMs = t + 700; }
        else { HostPulseButtons(kA); ui.nextPulseMs = t + 700; }          // a question on the way: its first answer
        break;
    case Walk::ToLounge:
        if (frame == kFrame) { ui.walk = Walk::None; break; }
        if (frame == kOnlineMenu) { menu_host::ShowFrame(ctx, base, kFrame); ui.nextPulseMs = t + 1500; }
        break;
    default:
        break;
    }
}

// The lounge's upkeep: pings, the host's checks of its members, its record and snapshots.
void Upkeep()
{
    std::vector<Out> out;
    bool closed = false, rejoin = false;
    {
        std::lock_guard l(g_m);
        const int64_t t = NowMs();
        if (g.phase == Phase::Joining && t > g.joinUntilMs)
        {
            std::fprintf(stderr, "[lounge] %016llX did not answer\n", (unsigned long long)g.hostPeer);
            g.phase = Phase::Searching;
            rejoin = true;
        }
        if (g.phase == Phase::In && !g.host)
        {
            if (t >= ui.nextPingMs) { out.push_back({ g.hostPeer, Writer(Ping).b }); ui.nextPingMs = t + kPingMs; }
            if (t - g.hostHeardMs > kHostTimeoutMs && ui.walk == Walk::None)
            {
                g.phase = Phase::Off;
                g.closedWhy = "The lounge's host is gone.";
                closed = true;
            }
        }
        if (g.phase == Phase::In && g.host)
        {
            bool changed = false;
            for (size_t k = g.members.size(); k-- > 0;)
            {
                const Member& m = g.members[k];
                if (m.peer == Self() || t - m.heardMs <= kMemberTimeoutMs) continue;
                std::fprintf(stderr, "[lounge] %s timed out\n", m.name.c_str());
                const std::string name = m.name;
                const uint64_t peer = m.peer;
                UnseatLocked(peer);
                g.members.erase(g.members.begin() + std::ptrdiff_t(k));
                ChatLocked("LOUNGE", name + " left.", out);
                changed = true;
            }
            // a match whose second report never came (a player's game lost the way back): decided by the first
            for (int i = 0; i < kCabinets; ++i)
                if (g.cab[i].state == Playing && g.cab[i].reports == 1 && t - g.cab[i].firstReportMs > 30000)
                {
                    FinishLocked(i, out);
                    changed = true;
                }
            if (changed || t >= ui.nextSnapshotMs)
            {
                BroadcastLocked(SnapshotLocked(), out);
                ui.nextSnapshotMs = t + kSnapshotMs;
            }
            if (changed || t >= ui.nextPublishMs)
            {
                PublishLocked();
                ui.nextPublishMs = t + 20000;
            }
        }
    }
    SendAll(out);
    if (rejoin) Search();
    if (closed)
    {
        toast::Note n;
        n.title = "FIGHTING LOUNGE";
        n.lines = { "The lounge closed: its host left." };
        toast::Show(n, 5000);
    }
}

} // namespace

bool IsLoungeRecord(const mm::SessionRecord& record)
{
    for (const mm::Context& c : record.contexts)
        if (c.id == kLoungeContext) return true;
    return false;
}

void PeerMessage(uint64_t from, const uint8_t* data, size_t size)
{
    if (size < 2 || data[0] != kVersion) return;
    Reader r(data + 2, size - 2);
    const Msg type = Msg(data[1]);
    std::vector<Out> out;
    {
        std::lock_guard l(g_m);
        if (g.host && g.phase == Phase::In)
        {
            HostHandleLocked(from, type, r, out);
        }
        else if (from == g.hostPeer && (g.phase == Phase::In || g.phase == Phase::Joining))
        {
            g.hostHeardMs = NowMs();
            switch (type)
            {
            case Snapshot:
                ApplySnapshotLocked(r);
                if (g.phase == Phase::Joining)
                {
                    g.phase = Phase::In;
                    std::fprintf(stderr, "[lounge] in %016llX's lounge (%zu players)\n", (unsigned long long)from, g.members.size());
                }
                break;
            case ChatOut:
            {
                const std::string name = Clean(r.Str(), profiles::kMaxName), text = Clean(r.Str(), kChatLength);
                if (r.ok) AddChatLocked(name, text);
                break;
            }
            case Start:
            {
                const int i = r.U8();
                const uint32_t match = r.U32();
                const uint64_t opponent = r.U64();
                if (!r.ok || i >= kCabinets) break;
                g.startPending = true;
                g.matchCab = i;
                g.match = match;
                g.opponent = opponent;
                break;
            }
            case Closed:
                g.closedWhy = Clean(r.Str(), 60);
                g.phase = Phase::Off;
                g.members.clear();
                std::fprintf(stderr, "[lounge] closed: %s\n", g.closedWhy.c_str());
                break;
            default:
                break;
            }
        }
        else if (type == Hello && g.phase != Phase::In)
        {
            // someone found a lounge record of ours that is gone, or came while this game still searches
            Writer w(Closed);
            w.Str("That lounge is gone. Try again.");
            out.push_back({ from, w.b });
        }
    }
    SendAll(out);
}

void MatchWinner(int side)
{
    std::lock_guard l(g_m);
    if (g.matchCab < 0) return;
    g.winnerSide = side;
    std::fprintf(stderr, "[lounge] match end: side %d won\n", side);
}

void MatchOver(bool ended)
{
    std::lock_guard l(g_m);
    if (g.matchCab < 0) return;
    g.overPending = true;
    g.overEnded = ended;
}

void Frame(PPCContext& ctx, uint8_t* base)
{
    const std::string reported = menu_host::CurrentFrame(ctx, base);
    if (reported != ui.candidate) { ui.candidate = reported; ui.candidateFrames = 0; }
    else if (ui.candidateFrames < 8) ++ui.candidateFrames;
    if (ui.candidateFrames == 8 && ui.candidate.rfind("Frame", 0) == 0 && ui.candidate != ui.frame)
    {
        ui.frame = ui.candidate;
        ui.frameSinceMs = NowMs();
        if (ui.walk != Walk::None) std::fprintf(stderr, "[lounge] walk %d: frame %s\n", int(ui.walk), ui.frame.c_str());
    }
    const std::string& frame = ui.frame;

    // a match to walk to, and the way back
    {
        bool start = false, over = false;
        int cab = -1;
        uint32_t match = 0;
        uint64_t opponent = 0, winner = 0;
        {
            std::lock_guard l(g_m);
            if (g.startPending)
            {
                g.startPending = false;
                start = true;
                opponent = g.opponent;
                g.winnerSide = -1;
            }
            if (g.overPending)
            {
                g.overPending = false;
                over = true;
                cab = g.matchCab;
                match = g.match;
                // the side numbers follow the room: its host (the lower peer id, ranked::BeforeSearch) plays side 0
                const int mySide = Self() < g.opponent ? 0 : 1;
                if (g.winnerSide == 0 || g.winnerSide == 1) winner = g.winnerSide == mySide ? Self() : g.opponent;
                g.matchCab = -1;
            }
        }
        if (start)
        {
            ranked::PlanLoungeMatch(opponent);
            ui.walk = Walk::LeavePage;
            ui.walkUntilMs = NowMs() + 30000;
            ui.nextPulseMs = NowMs() + 300;
            std::fprintf(stderr, "[lounge] match against %016llX: walking to PLAYER MATCH\n", (unsigned long long)opponent);
            toast::Note n;
            n.title = "FIGHTING LOUNGE";
            n.lines = { "Both players are ready.", "Starting the match..." };
            toast::Show(n, 5000);
        }
        if (over)
        {
            Writer w(Result);
            w.U8(uint8_t(cab));
            w.U32(match);
            w.U64(winner);
            Request(w);
            ui.walk = Walk::Back;
            ui.walkUntilMs = NowMs() + 60000;
            ui.nextPulseMs = NowMs() + 4000;              // the result screens first
            std::fprintf(stderr, "[lounge] cabinet match over (winner %016llX): walking back\n", (unsigned long long)winner);
        }
    }
    WalkFrame(ctx, base, frame);

    // the page
    if (frame == kFrame)
    {
        if (!ui.pageUp)
        {
            ui.pageUp = true;
            ui.settle = 2;
            ui.drawn = false;
            ui.top = 0;
            ui.prev = HostControllerButtons();           // the A that opened the page is not ours
            Enter();
        }
        PageFrame(ctx, base);
    }
    else if (ui.pageUp && frame.rfind("Frame", 0) == 0)
    {
        ui.pageUp = false;
        // left by B (not on the way to a match): out of the lounge
        if (ui.walk == Walk::None) LeaveLounge();
    }
    if (!ui.pageUp) ui.mask = 0xFFFF;
    // out of ONLINE MODE altogether
    if (frame == "FrameMainMenu" && ui.walk == Walk::None) LeaveLounge();
    Upkeep();
}

uint16_t GameButtonMask() { return ui.mask.load(); }

void KeyHints(PPCContext& ctx, uint8_t* base)
{
    if (!ui.pageUp || !ui.drawn) return;
    const int c = menu_host::Cursor(ctx, base, kMenu);
    if (c >= 0 && ui.top + c >= 1) menu_host::KeyHint(ctx, base, kHintX, "LEAVE CABINET");
}

} // namespace lounge
