// Ranked Match extras: rematch, quits (ranked_match.h).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "ranked_match.h"
#include "lounge.h"
#include "community_net.h"
#include "net_transport.h"
#include "sys_ui.h"
#include "toast.h"
#include "xlive.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

void XSessionRepublish();   // xsession.cpp: the hosted sessions' records again (a reservation ended)

namespace ranked {
namespace {

using clock = std::chrono::steady_clock;
constexpr uint32_t kArbitration = 0x10;                  // XSESSION_CREATE: a ranked match
constexpr uint32_t kMatchmaking = 0x08;                  // XSESSION_CREATE: a match's session (a PLAYER MATCH room too)
// The opponent left when its packets had stopped this long by the time the game ended or deleted the match's session
// (a match that ends normally ends at both games together; a game whose opponent vanished first waits out its own
// disconnect timeout).
constexpr int64_t kLeftSilenceMs = 3000;
constexpr auto kAskTime = std::chrono::seconds(20);      // the rematch question stays open this long
constexpr auto kJoinWait = std::chrono::seconds(15);     // the rematch guest looks for the host's room this long
constexpr auto kReserveTime = std::chrono::seconds(30);  // the host's room waits for the guest alone this long
constexpr uint32_t kReserveHi = 0x7E5E0001u, kReserveLo = 0x7E5E0002u;   // the reservation's contexts (port's own ids)
constexpr uint8_t kVersion = 1, kAccept = 1, kDecline = 2;
constexpr uint16_t kButtonX = 0x4000, kButtonY = 0x8000;

enum class Ask { None, Asking, Agreed, Declined };

struct State
{
    // the ranked match being played
    uint64_t session = 0;
    bool started = false, ended = false, isRematch = false;
    // the rematch question
    Ask ask = Ask::None;
    uint64_t askPeer = 0, askSession = 0;
    bool mine = false, theirs = false;
    clock::time_point askEnd{};
    std::string why;                                     // why there is no rematch (Declined)
    // an agreed rematch: the next ranked match against this peer is it
    uint64_t rematchPeer = 0;
    bool loungeNext = false, isLounge = false;           // that next match is a FIGHTING LOUNGE cabinet's (this one is)
    // the room this game hosts for the rematch
    uint64_t reservedFor = 0;
    clock::time_point reservedEnd{};
    // an answer that came before this game asked (the other game's match ended first)
    uint64_t earlyPeer = 0, earlySession = 0;
    uint8_t earlyKind = 0;
    // the opponent's ranked record, as the question shows it
    uint64_t conductPeer = 0;
    std::string conductText;
    // this player's own cooldown, as the server last said (unix seconds) and when it was asked
    uint64_t cooldownUntil = 0;
    clock::time_point cooldownAsked{};
};
std::mutex g_m;
State g;

std::shared_ptr<community::Client> Conduct()
{
    std::shared_ptr<community::Client> server = net::Community();
    return server && server->KeepsConduct() ? server : nullptr;
}

void Send(uint64_t peer, uint8_t kind, uint64_t session)
{
    uint8_t msg[10] = { kVersion, kind };
    std::memcpy(msg + 2, &session, 8);
    XSocketSendRematch(peer, msg, sizeof(msg));
}

void Note(const std::string& title, std::vector<std::string> lines, int ms)
{
    toast::Note n;
    n.title = title;
    n.lines = std::move(lines);
    toast::Show(n, ms);
}

std::string RecordText(const community::Conduct& c)
{
    char text[96];
    if (c.matches == 0) return "No ranked matches on this server yet.";
    const unsigned rate = unsigned((uint64_t(c.quits) * 100 + c.matches / 2) / c.matches);
    std::snprintf(text, sizeof(text), "Left %u of %u ranked matches (%u%%).", c.quits, c.matches, rate);
    return text;
}

// Both answered yes (under g_m).
void AgreeLocked()
{
    g.ask = Ask::Agreed;
    g.rematchPeer = g.askPeer;
    g.askEnd = clock::now() + std::chrono::minutes(1);  // the next ranked search must come by then
    std::fprintf(stderr, "[ranked] rematch with %016llX agreed\n", (unsigned long long)g.askPeer);
}

void DeclineLocked(const std::string& why)
{
    g.ask = Ask::Declined;
    g.why = why;
    std::fprintf(stderr, "[ranked] no rematch: %s\n", why.c_str());
}

// The match's session ended (`ended`: XSessionEnd; else deleted before the end).
void Finish(uint64_t session, bool ended)
{
    uint64_t peer = 0;
    bool wasRematch = false, wasLounge = false;
    {
        std::lock_guard l(g_m);
        if (g.session != session || !g.started || g.ended) return;
        g.ended = true;
        peer = XSocketMatchPeer();
        wasRematch = g.isRematch && peer == g.rematchPeer;
        if (g.isRematch) g.rematchPeer = 0;
        wasLounge = g.isLounge;
    }
    if (wasLounge) lounge::MatchOver(ended);
    if (!peer) return;
    const int64_t silence = XSocketSilenceMs(peer);
    if (silence >= kLeftSilenceMs)
    {
        std::fprintf(stderr, "[ranked] match %016llX: opponent %016llX silent for %lld ms - it left\n", (unsigned long long)session,
                     (unsigned long long)peer, (long long)silence);
        Send(peer, kDecline, session);
        const std::shared_ptr<community::Client> server = Conduct();
        if (server) server->ReportLeft(session);
        Note("OPPONENT LEFT", { "Your opponent left the match before its end.",
                                server ? "The server counts it as their quit." : "" }, 6000);
        return;
    }
    if (!ended || wasLounge) return;                     // left (or gave up on) the match, or the lounge's: nothing to ask
    if (wasRematch)
    {
        Note("REMATCH OVER", { "The next search finds a new opponent." }, 5000);
        return;
    }
    std::lock_guard l(g_m);
    g.ask = Ask::Asking;
    g.askPeer = peer;
    g.askSession = session;
    g.mine = g.theirs = false;
    g.askEnd = clock::now() + kAskTime;
    if (g.conductPeer != peer) g.conductText.clear();
    if (g.earlyPeer == peer && g.earlySession == session)
    {
        if (g.earlyKind == kAccept) g.theirs = true;
        else DeclineLocked("Your opponent declined.");
    }
    g.earlyPeer = 0;
    std::fprintf(stderr, "[ranked] match %016llX over: rematch question for %016llX\n", (unsigned long long)session,
                 (unsigned long long)peer);
}

// This player's ranked cooldown (a community server that keeps conduct): told when they search during one.
void CheckCooldown()
{
    const std::shared_ptr<community::Client> server = Conduct();
    if (!server) return;
    uint64_t until = 0;
    {
        std::lock_guard l(g_m);
        if (clock::now() - g.cooldownAsked < std::chrono::seconds(15)) until = g.cooldownUntil;
        else
        {
            g.cooldownAsked = clock::now();
            until = ~0ull;
        }
    }
    if (until == ~0ull)
    {
        community::Conduct c;
        until = server->GetConduct(0, c, 2000) == 1 ? c.cooldownUntil : 0;
        std::lock_guard l(g_m);
        g.cooldownUntil = until;
    }
    const int64_t left = int64_t(until) - int64_t(std::time(nullptr));
    if (left <= 0) return;
    char text[96];
    std::snprintf(text, sizeof(text), "Ranked matchmaking opens again in %lld:%02lld.", (long long)(left / 60), (long long)(left % 60));
    Note("RANKED COOLDOWN", { "You left a ranked match before its end.", text }, 8000);
    std::fprintf(stderr, "[ranked] search during a ranked cooldown (%lld s left)\n", (long long)left);
}

// The question's note: shown when it opens, updated as its seconds run (main thread).
struct Prompt
{
    bool shown = false;
    std::string text;
    uint16_t buttons = 0;
} g_prompt;

std::vector<std::string> Wrap(const std::string& text, size_t width)
{
    std::vector<std::string> lines;
    size_t at = 0;
    while (at < text.size() && lines.size() < 4)
    {
        size_t end = (std::min)(text.size(), at + width);
        if (end < text.size())
        {
            const size_t space = text.rfind(' ', end);
            if (space != std::string::npos && space > at) end = space;
        }
        lines.push_back(text.substr(at, end - at));
        at = end;
        while (at < text.size() && text[at] == ' ') ++at;
    }
    return lines;
}

} // namespace

void SessionStarted(uint64_t session, uint32_t flags)
{
    std::lock_guard l(g_m);
    // a FIGHTING LOUNGE match is a PLAYER MATCH room's (unranked): followed like a ranked one
    if (!(flags & kArbitration) && !(g.loungeNext && (flags & kMatchmaking))) return;
    g.session = session;
    g.started = true;
    g.ended = false;
    g.isRematch = g.rematchPeer != 0;
    g.isLounge = g.isRematch && g.loungeNext;
    g.loungeNext = false;
    g.reservedFor = 0;                                   // the guest came: the room is full anyway
}

bool LoungeSessionLocked(uint64_t session) { return g.isLounge && g.session == session; }

void SessionEnded(uint64_t session, uint32_t flags, uint32_t)
{
    bool lounge;
    {
        std::lock_guard l(g_m);
        lounge = LoungeSessionLocked(session);
    }
    if ((flags & kArbitration) || lounge) Finish(session, true);
}

void SessionDeleted(uint64_t session, uint32_t flags, uint32_t)
{
    bool lounge;
    {
        std::lock_guard l(g_m);
        lounge = LoungeSessionLocked(session);
    }
    if (!(flags & kArbitration) && !lounge) return;
    Finish(session, false);
    std::lock_guard l(g_m);
    if (g.session == session) { g.session = 0; g.started = false; }
}

void OpponentKnown(uint64_t opponent)
{
    const std::shared_ptr<community::Client> server = Conduct();
    if (!server || !opponent) return;
    std::thread([server, opponent] {
        community::Conduct c;
        if (server->GetConduct(opponent, c, 3000) != 1) return;
        const std::string text = RecordText(c);
        {
            std::lock_guard l(g_m);
            g.conductPeer = opponent;
            g.conductText = text;
        }
        Note("RANKED OPPONENT", { text }, 6000);
    }).detach();
}

SearchPlan BeforeSearch(bool canWait)
{
    SearchPlan plan;
    if (canWait)
    {
        CheckCooldown();
        for (;;)                                         // the question first (the game is on its search screen)
        {
            {
                std::lock_guard l(g_m);
                if (g.ask != Ask::Asking || clock::now() >= g.askEnd) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    std::lock_guard l(g_m);
    if (g.ask != Ask::Agreed) return plan;
    g.ask = Ask::None;
    if (clock::now() >= g.askEnd)                       // agreed, then the player went elsewhere
    {
        g.rematchPeer = 0;
        return plan;
    }
    plan.peer = g.rematchPeer;
    plan.kind = net::LocalPeer() < plan.peer ? SearchPlan::Host : SearchPlan::Join;
    if (plan.kind == SearchPlan::Host)
    {
        g.reservedFor = plan.peer;
        g.reservedEnd = clock::now() + kReserveTime;
    }
    std::fprintf(stderr, "[ranked] rematch search: %s %016llX\n", plan.kind == SearchPlan::Host ? "hosting for" : "joining",
                 (unsigned long long)plan.peer);
    return plan;
}

void JoinNotFound()
{
    {
        std::lock_guard l(g_m);
        g.rematchPeer = 0;
        g.loungeNext = false;
    }
    std::fprintf(stderr, "[ranked] the rematch room never showed up\n");
    Note("NO REMATCH", { "Your opponent's room was not found.", "Searching for a new opponent." }, 5000);
}

void PlanLoungeMatch(uint64_t peer)
{
    std::lock_guard l(g_m);
    g.ask = Ask::Agreed;
    g.rematchPeer = peer;
    g.askEnd = clock::now() + std::chrono::minutes(2);  // the menus are walked to the search by then
    g.loungeNext = true;
    std::fprintf(stderr, "[ranked] lounge match with %016llX planned\n", (unsigned long long)peer);
}

bool LoungePlanned()
{
    std::lock_guard l(g_m);
    return g.loungeNext && g.ask == Ask::Agreed;
}

void AddReservation(std::vector<mm::Context>& contexts)
{
    std::lock_guard l(g_m);
    if (!g.reservedFor) return;
    contexts.push_back({ kReserveHi, uint32_t(g.reservedFor >> 32) });
    contexts.push_back({ kReserveLo, uint32_t(g.reservedFor) });
}

bool ReservedForOther(const mm::SessionRecord& record)
{
    uint64_t hi = 0, lo = 0;
    bool reserved = false;
    for (const mm::Context& c : record.contexts)
    {
        if (c.id == kReserveHi) { hi = c.value; reserved = true; }
        if (c.id == kReserveLo) lo = c.value;
    }
    return reserved && (hi << 32 | lo) != net::LocalPeer();
}

void PeerMessage(uint64_t from, const uint8_t* data, size_t size)
{
    if (size < 10 || data[0] != kVersion || (data[1] != kAccept && data[1] != kDecline)) return;
    uint64_t session;
    std::memcpy(&session, data + 2, 8);
    std::lock_guard l(g_m);
    if (g.ask != Ask::Asking || from != g.askPeer || session != g.askSession)
    {
        g.earlyPeer = from;                              // kept for the question this game is about to ask
        g.earlySession = session;
        g.earlyKind = data[1];
        return;
    }
    if (data[1] == kDecline) { DeclineLocked("Your opponent declined."); return; }
    g.theirs = true;
    if (g.mine) AgreeLocked();
}

void Frame()
{
    // the server's notices (an admin's message, a ranked cooldown) - not over the question
    if (!g_prompt.shown)
        if (const std::shared_ptr<community::Client> server = net::Community())
        {
            std::string text;
            if (server->TakeNotice(text)) Note("SERVER", Wrap(text, 52), 9000);
        }

    bool republish = false;
    Ask ask;
    std::string text, why;
    bool mine = false;
    {
        std::lock_guard l(g_m);
        if (g.reservedFor && clock::now() >= g.reservedEnd)
        {
            g.reservedFor = 0;                           // the guest never came: the room is open to anyone
            republish = true;
        }
        if (g.ask == Ask::Asking && clock::now() >= g.askEnd)
            DeclineLocked(g.mine ? "Your opponent did not answer." : "No answer.");
        ask = g.ask;
        mine = g.mine;
        why = g.why;
        if (ask == Ask::Asking)
        {
            const long long seconds = (long long)std::chrono::duration_cast<std::chrono::seconds>(g.askEnd - clock::now()).count() + 1;
            text = (g.conductText.empty() ? "" : g.conductText + "\n") +
                   (mine ? "Waiting for your opponent...  " : "Y / click: yes     X / right-click: no     ") + std::to_string(seconds);
        }
    }
    if (republish) XSessionRepublish();

    if (ask != Ask::Asking)
    {
        if (g_prompt.shown)
        {
            g_prompt.shown = false;
            if (ask == Ask::Agreed) Note("REMATCH", { "Both players accepted.", "Finding your opponent's room..." }, 6000);
            else Note("NO REMATCH", { why }, 4000);
        }
        if (ask == Ask::Declined)
        {
            std::lock_guard l(g_m);
            if (g.ask == Ask::Declined) g.ask = Ask::None;
        }
        g_prompt.buttons = HostControllerButtons();
        return;
    }

    // the answer: Y / X (pressed now, not held from before), or a click on the note
    const uint16_t buttons = HostControllerButtons();
    const uint16_t pressed = buttons & ~g_prompt.buttons;
    g_prompt.buttons = buttons;
    const int click = g_prompt.shown ? toast::TakeClick() : 0;
    int answer = 0;                                      // 1 yes, 2 no
    if ((pressed & kButtonY) || click == 1) answer = 1;
    else if ((pressed & kButtonX) || click == 2) answer = 2;
    if (answer && !mine)
    {
        std::lock_guard l(g_m);
        if (g.ask == Ask::Asking)
        {
            Send(g.askPeer, answer == 1 ? kAccept : kDecline, g.askSession);
            if (answer == 2) DeclineLocked("You declined.");
            else
            {
                g.mine = true;
                if (g.theirs) AgreeLocked();
            }
        }
        return;                                          // the note follows next frame
    }

    toast::Note note;
    note.title = "REMATCH?";
    note.clickable = !mine;
    note.lines.push_back("Play the same opponent once more?");
    for (size_t at = 0; at <= text.size();)
    {
        const size_t end = text.find('\n', at);
        note.lines.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
        if (end == std::string::npos) break;
        at = end + 1;
    }
    const std::string shown = note.lines.back() + (note.clickable ? "1" : "0");
    if (!g_prompt.shown || !toast::Showing())
    {
        toast::Show(note, 0);
        g_prompt.shown = true;
    }
    else if (shown != g_prompt.text) toast::Update(note);
    g_prompt.text = shown;
}

uint16_t GameButtonMask()
{
    std::lock_guard l(g_m);
    return g.ask == Ask::Asking && !g.mine ? uint16_t(~(kButtonX | kButtonY)) : uint16_t(0xFFFF);
}

bool PromptShowing() { return g_prompt.shown; }

} // namespace ranked
