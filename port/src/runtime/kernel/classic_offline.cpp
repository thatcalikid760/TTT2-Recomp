// CLASSIC OFFLINE (classic_offline.h). MEASURED 2026-10-24:
//  - sub_82276760(fighter) answers whether fighter 0..62 is selectable (character select asks it for each slot);
//    a "no" keeps the portrait but the cursor skips it (Unknown's slot shows greyed). Fighter ids follow the exe's
//    name table (Paul Phoenix, Marshall Law, ...): 21 Anna, 25 Bruce, 44 Unknown checked on screen.
//  - sub_825DC118 switches the game mode to state_ending_init (its only caller: the outcome after a battle) - the
//    ARCADE clear, before the fighter's ending.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "classic_offline.h"
#include "menu_host.h"
#include "profiles.h"
#include "sys_ui.h"
#include "toast.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

extern "C" PPC_FUNC(__imp__sub_82276760);
extern "C" PPC_FUNC(__imp__sub_825DC118);

namespace classic_offline {
namespace {

struct Fighter { int id; const char* name; };
// Open from the start: 0 Paul, 1 Law, 2 Lei, 3 King, 4 Yoshimitsu, 5 Nina, 6 Hwoarang, 7 Xiaoyu, 8 Christie, 9 Jin,
// 10 Julia, 12 Bryan, 16 Steve, 17 Marduk, 24 Asuka, 29 Feng, 31 Lili, 32 Dragunov, 34 Bob, 37 Leo, 38 Lars, 39 Alisa.
// The others, one per ARCADE clear:
constexpr Fighter kUnlocks[] = {
    { 21, "Anna" }, { 25, "Bruce" }, { 26, "Baek" }, { 23, "Ganryu" }, { 22, "Wang" }, { 20, "Roger Jr." },
    { 11, "Kuma" }, { 43, "Panda" }, { 30, "Armor King" }, { 33, "Eddy" }, { 35, "Zafina" }, { 36, "Miguel" },
    { 15, "Lee" }, { 28, "Raven" }, { 51, "Michelle" }, { 45, "Kunimitsu" }, { 47, "Forest Law" }, { 18, "Mokujin" },
    { 19, "JACK-6" }, { 52, "P-JACK" }, { 53, "Alex" }, { 58, "Tiger Jackson" }, { 49, "Combot" }, { 46, "Slim Bob" },
    { 48, "Miharu" }, { 57, "Sebastian" }, { 55, "Violet" }, { 50, "Angel" }, { 27, "Devil Jin" }, { 14, "Kazuya" },
    { 13, "Heihachi" }, { 42, "Jun" }, { 44, "Unknown" }, { 40, "Jinpachi" }, { 54, "Ancient Ogre" }, { 41, "True Ogre" },
    { 56, "Dr. Bosconovitch" },
};
constexpr int kTotal = int(sizeof(kUnlocks) / sizeof(kUnlocks[0]));
constexpr uint16_t kButtonX = 0x4000;
constexpr uint32_t kHintX = 57383;                 // the key help's X icon (as the server list's REMOVE)
constexpr int kSettle = 30;                        // frames a menu must stay to count

std::atomic<bool> g_active{ false };
std::atomic<bool> g_hint{ false };
std::mutex g_mutex;
uint64_t g_xuid = 0;                               // whose progress g_unlocked is
int g_unlocked = 0;

std::filesystem::path File(uint64_t xuid)
{
    const wchar_t* overridePath = _wgetenv(L"TTT2_SAVE_ROOT");
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    std::filesystem::path root;
    if (overridePath && *overridePath) root = overridePath;
    else if (local && *local) root = std::filesystem::path(local) / L"TTT2Recomp" / L"saves";
    else return {};
    wchar_t hex[17];
    swprintf(hex, 17, L"%016llX", static_cast<unsigned long long>(xuid));
    return root / hex / L"classic_offline.ini";
}

// The signed-in profile's unlocked count (under g_mutex); -1 when nobody is signed in.
int UnlockedLocked()
{
    const uint64_t xuid = profiles::ActiveXuid();
    if (!xuid) return -1;
    if (xuid != g_xuid)
    {
        g_xuid = xuid;
        g_unlocked = 0;
        std::ifstream in(File(xuid));
        std::string line;
        while (in && std::getline(in, line))
            if (line.rfind("Unlocked=", 0) == 0) g_unlocked = std::clamp(std::atoi(line.c_str() + 9), 0, kTotal);
    }
    return g_unlocked;
}

void SaveLocked()
{
    const std::filesystem::path path = File(g_xuid);
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    out << "[ClassicOffline]\nUnlocked=" << g_unlocked << "\n";
}

bool Locked(uint32_t fighter)
{
    std::lock_guard l(g_mutex);
    const int unlocked = UnlockedLocked();
    if (unlocked < 0) return false;
    for (int i = unlocked; i < kTotal; ++i)
        if (kUnlocks[i].id == int(fighter)) return true;
    return false;
}

void Note(const std::string& title, std::vector<std::string> lines, int ms)
{
    toast::Note n;
    n.title = title;
    n.lines = std::move(lines);
    toast::Show(n, ms);
}

} // namespace

void Frame(PPCContext& ctx, uint8_t* base)
{
    static int mainFrames = 0, awayFrames = 0;
    static bool away = false;
    static uint16_t lastButtons = 0;
    const bool mainMenu = menu_host::CurrentFrame(ctx, base) == "FrameMainMenu";
    mainFrames = mainMenu ? (std::min)(mainFrames + 1, kSettle) : 0;
    awayFrames = mainMenu ? 0 : (std::min)(awayFrames + 1, kSettle);
    const uint16_t buttons = HostControllerButtons();
    const uint16_t pressed = buttons & ~lastButtons;
    lastButtons = buttons;

    // back on the main menu after the mode: the normal roster again
    if (g_active && awayFrames >= kSettle) away = true;
    if (g_active && away && mainFrames >= kSettle)
    {
        g_active = false;
        away = false;
        std::fprintf(stderr, "[classic] left CLASSIC OFFLINE\n");
    }

    int unlocked;
    {
        std::lock_guard l(g_mutex);
        unlocked = UnlockedLocked();
    }
    const bool offer = !g_active && mainFrames >= kSettle && unlocked >= 0 && unlocked < kTotal && !SysUiOwnsInput() &&
                       menu_host::Cursor(ctx, base, "MenuMainMenu") == 1;   // OFFLINE MODE
    g_hint = offer;
    if (!offer) return;
    menu_host::KeyHint(ctx, base, kHintX, "CLASSIC OFFLINE");
    if (!(pressed & kButtonX)) return;
    g_active = true;
    away = false;
    HostPulseA();                                       // OFFLINE MODE, as its own A would enter it
    std::fprintf(stderr, "[classic] CLASSIC OFFLINE (%d of %d fighters unlocked)\n", unlocked, kTotal);
    Note("CLASSIC OFFLINE", { std::to_string(unlocked) + " of " + std::to_string(kTotal) + " fighters unlocked.",
                              "Clear ARCADE to unlock the next one." }, 6000);
}

uint16_t GameButtonMask() { return g_hint ? uint16_t(~kButtonX) : uint16_t(0xFFFF); }

} // namespace classic_offline

// Is fighter r3 selectable? -> r3. In CLASSIC OFFLINE the fighters still to unlock are not.
PPC_FUNC(sub_82276760)
{
    const uint32_t fighter = ctx.r3.u32;
    __imp__sub_82276760(ctx, base);
    if (classic_offline::g_active && ctx.r3.u32 && classic_offline::Locked(fighter)) ctx.r3.u64 = 0;
}

// The game mode goes to its ending (an ARCADE clear): in CLASSIC OFFLINE the next fighter opens.
PPC_FUNC(sub_825DC118)
{
    using namespace classic_offline;
    __imp__sub_825DC118(ctx, base);
    std::fprintf(stderr, "[classic] ARCADE cleared%s\n", g_active ? " in CLASSIC OFFLINE" : "");
    if (!g_active) return;
    std::string name;
    int unlocked;
    {
        std::lock_guard l(g_mutex);
        unlocked = UnlockedLocked();
        if (unlocked < 0 || unlocked >= kTotal) return;
        name = kUnlocks[unlocked].name;
        unlocked = ++g_unlocked;
        SaveLocked();
    }
    std::fprintf(stderr, "[classic] %s unlocked (%d of %d)\n", name.c_str(), unlocked, kTotal);
    if (unlocked < kTotal)
        Note("NEW CHALLENGER", { name + " is now playable in CLASSIC OFFLINE.",
                                 std::to_string(unlocked) + " of " + std::to_string(kTotal) + " fighters unlocked." }, 8000);
    else
        Note("CLASSIC OFFLINE COMPLETE", { name + " is now playable.", "Every fighter is unlocked." }, 10000);
}
