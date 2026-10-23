// PC menu extensions (plan and reverse-engineering notes: port/MENU_PLAN.md).
//
// TTT2's menus are BX01 binary-XML documents (frames, menus, rows, value lists) whose event
// handlers are precompiled Xtal scripts calling `lib::` natives. This module
//  - edits the Options / main-menu document as the game loads it (menu_doc.cpp): DISPLAY MODE
//    and RESOLUTION rows in Display Settings, EXIT in the main menu with its confirmation;
//  - registers the natives those rows and the dialog call, next to the game's own options natives;
//  - initialises the new rows' value cursors whenever Display Settings initialises MOTION BLUR's;
//  - supplies the confirmation's text and YES / NO labels, and quits on YES;
//  - Controller Setup's KEYBOARD mode: player 1's keyboard keys shown and changed in the button
//    panel (keyui below, keyboard_keys.h).
// It also hosts the system UI (sys_ui.h: Select Profile and the new profile's name dialog).
// Diagnostics: TTT2_MENU_DUMP=1 writes every BX01 document (and the edited one) to
// C:/ttt2/port/menu_bin/; TTT2_TEXT_TRACE=1 logs each text id the game looks up (first time, with
// the caller). Development: TTT2_MENU_OVERRIDE=1 loads a document prepared with
// port/tools/menu_proto.py from C:/ttt2/port/menu_override/<fnv1a>.bin instead of editing.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "kernel_shared.h"
#include "guest_heap.h"
#include "display_settings.h"
#include "menu_doc.h"
#include "sys_ui.h"
#include "profiles.h"
#include "keyboard_keys.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>
#include "discord_presence.h"
#include "voice_chat.h"
#include "updater.h"
#include "button_prompts.h"

extern "C" PPC_FUNC(__imp__sub_82346060);
extern "C" PPC_FUNC(__imp__sub_82A8C170);
extern "C" PPC_FUNC(__imp__sub_823756A8);
extern "C" PPC_FUNC(__imp__sub_8256CD28);
extern "C" PPC_FUNC(__imp__sub_8237F5D0);
extern "C" PPC_FUNC(__imp__sub_8241E7A8);
extern "C" PPC_FUNC(__imp__sub_82623858);
extern "C" PPC_FUNC(__imp__sub_8241B9D0);
extern "C" PPC_FUNC(__imp__sub_8256C708);
extern "C" PPC_FUNC(__imp__sub_82374700);
extern "C" PPC_FUNC(__imp__sub_823747C0);
extern void (*g_uiFrameHook)(PPCContext& ctx, uint8_t* base);
void XamNotifyEnqueueEvent(uint32_t id, uint32_t param);

namespace {
bool EnvFlag(const char* name)
{
    const char* e = std::getenv(name);
    return e && *e == '1';
}
bool MenuDump() { static const bool on = EnvFlag("TTT2_MENU_DUMP"); return on; }
bool MenuOverride() { static const bool on = EnvFlag("TTT2_MENU_OVERRIDE"); return on; }

uint32_t Fnv1a(const uint8_t* p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

void SaveFile(const char* path, const void* data, size_t size)
{
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(data, 1, size, f); std::fclose(f); }
}

// A copy of `bytes` in never-freed guest memory (`prefix` zero bytes in front); guest address.
uint32_t GuestCopy(uint8_t* base, const void* bytes, size_t size, uint32_t prefix = 0)
{
    auto* host = static_cast<uint8_t*>(g_virtualHeap.Alloc(prefix + size + 16));
    std::memset(host, 0, prefix);
    std::memcpy(host + prefix, bytes, size);
    return static_cast<uint32_t>(host + prefix - base);
}
uint32_t GuestCString(uint8_t* base, const char* text, uint32_t prefix = 0)
{
    return GuestCopy(base, text, std::strlen(text) + 1, prefix);
}

// A string as the game's text lookup returns it: data 4 bytes into a table entry whose last
// header byte is the length.
uint32_t TableText(uint8_t* base, const char* text)
{
    const uint32_t addr = GuestCString(base, text, 4);
    GWrite8(base, addr - 1, static_cast<uint8_t>(std::strlen(text)));
    return addr;
}

std::string GuestString(uint8_t* base, uint32_t addr, size_t max = 260)
{
    std::string s;
    for (size_t i = 0; addr && i < max && base[addr + i]; ++i) s.push_back(static_cast<char>(base[addr + i]));
    return s;
}

// A scratch frame below the current guest stack pointer for calling guest functions from a
// hook; restores r1 on destruction. The ABI back chain is written.
struct GuestFrame
{
    PPCContext& ctx;
    uint32_t saved;
    GuestFrame(PPCContext& c, uint8_t* base, uint32_t bytes) : ctx(c), saved(c.r1.u32)
    {
        const uint32_t sp = (saved - bytes) & ~0xFu;
        GWrite32(base, sp, saved);
        ctx.r1.u32 = sp;
    }
    ~GuestFrame() { ctx.r1.u32 = saved; }
    uint32_t sp() const { return ctx.r1.u32; }
};

// Menu widget by name (sub_8256C160, as lib::MenuSelectValueSetCursor looks it up) or 0.
uint32_t FindWidget(PPCContext& ctx, uint8_t* base, uint32_t nameAddr)
{
    ctx.r3.u64 = nameAddr;
    sub_8256C160(ctx, base);
    return ctx.r3.u32;
}

// lib::MenuSwitchFrame(name) (impl sub_8256BF88: name is a C string).
void SwitchFrame(PPCContext& ctx, uint8_t* base, const char* name)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestCString(base, name);
    sub_8256BF88(ctx, base);
}

// ---- system UI (sys_ui.h, PROFILE_PLAN.md) ---------------------------------------------------
// The console-side screens. Select Profile opens when START / A is pressed on the title logo
// while nobody is signed in, drawn with FrameSysMenu<N> (menu_doc.cpp: N = 2..9 rows in Display
// Settings' style; the list uses the frame with exactly its row count); a new profile is named in
// the game's confirmation window. Everything here runs on the game's main thread at the end of
// VdSwap. Two-argument menu natives take the value first and the widget name second.

// Guest copies of strings, one per distinct text (texts change only on input; never freed).
uint32_t GuestStr(uint8_t* base, const std::string& s)
{
    static std::map<std::string, uint32_t> pool;
    const auto it = pool.find(s);
    if (it != pool.end()) return it->second;
    const uint32_t addr = GuestCString(base, s.c_str());
    pool.emplace(s, addr);
    return addr;
}
bool g_hostText = false;                              // MenuSetText is writing (the hook passes it on)
void MenuSetText(PPCContext& ctx, uint8_t* base, const std::string& widget, const std::string& text)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, text);
    ctx.r4.u64 = GuestStr(base, widget);
    g_hostText = true;
    sub_8256C708(ctx, base);                          // lib::MenuEntitySetText(text, widget)
    g_hostText = false;
}
void MenuMovieVisible(PPCContext& ctx, uint8_t* base, const std::string& movie, bool visible)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, movie);
    ctx.r4.u64 = visible ? 1 : 0;
    sub_8256D518(ctx, base);                          // lib::MenuIFLumenSetMovieVisible(movie, bool)
}
void MenuTelopTarget(PPCContext& ctx, uint8_t* base, const std::string& widget)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, widget);
    sub_825719A8(ctx, base);                          // lib::MenuTelopSetTargetEntity(widget)
}
void MenuSetCursor(PPCContext& ctx, uint8_t* base, const std::string& menu, int index)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = static_cast<uint32_t>(index);
    ctx.r4.u64 = GuestStr(base, menu);
    sub_8256C928(ctx, base);                          // lib::MenuContainerSetCursor(index, menu)
}

void MenuMoviePlay(PPCContext& ctx, uint8_t* base, const std::string& movie, const std::string& label, bool force)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, movie);
    ctx.r4.u64 = GuestStr(base, label);
    ctx.r5.u64 = force ? 1 : 0;
    sub_8256D5C8(ctx, base);                          // lib::MenuIFLumenMoviePlayFromLabelAndStop
}
void MenuTextHide(PPCContext& ctx, uint8_t* base, const std::string& widget, bool hide)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = hide ? 1 : 0;
    ctx.r4.u64 = GuestStr(base, widget);
    sub_8256C758(ctx, base);                          // lib::MenuEntitySetTextHide(bool, widget)
}
void MenuTickCount(PPCContext& ctx, uint8_t* base, const std::string& widget, uint32_t count)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = count;
    ctx.r4.u64 = GuestStr(base, widget);
    sub_8256C2B0(ctx, base);                          // lib::MenuSetTickCount(count, widget)
}
// lib::MenuSoundKeyOn(id) with the menus' SoundIdMenuCursor / Decide / Cancel (natives defined
// by sub_825A5098).
constexpr uint32_t kSoundCursor = 8426, kSoundCancel = 8427, kSoundDecide = 8428;
void MenuSound(PPCContext& ctx, uint8_t* base, uint32_t id)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = id;
    sub_8259E948(ctx, base);
}

bool keyui_Foreground()
{
    DWORD process = 0;
    const HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &process);
    return w && process == GetCurrentProcessId();
}

namespace sysui {

enum class Screen { None, Profiles, Name, Graphics, Voice, Update };
constexpr int kRows = 9;
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kA = 0x1000, kB = 0x2000;
constexpr uint32_t kNameTextId = 0x7F02;              // the name dialog's message (text lookup hook)

struct Row { std::string label, value, help; };
struct Ui
{
    Screen screen = Screen::None;
    int setupFrames = 0;            // frames left before the rows are written (after a switch)
    bool releasing = false;         // closed: keep the controller until every button is up
    bool continueAfter = false;     // then press START for the game (the title continues)
    int cursor = 0;
    std::vector<Row> rows, shown;   // wanted / last written
    int shownCursor = -1;
    int frameRows = 0;              // FrameSysMenu<N> on screen (0 = none yet)
    int settle = 0;                 // frames until the cursor is drawn again (after the menu's OnFocus)
    std::string name, note;         // the name dialog
    bool suggested = false;         // `name` is the suggestion: the first typed character replaces it
    uint16_t prev = 0, repeatKey = 0;
    int64_t repeatAt = 0;
};
Ui g_ui;
std::atomic<bool> g_owns{ false }, g_textEntry{ false }, g_signInRequest{ false };
std::atomic<bool> g_switchProfile{ false };   // Options -> SWITCH PROFILE confirmed: sign out on the logo
std::atomic<int64_t> g_titleLogoMs{ 0 };
std::mutex g_charMutex;
std::vector<uint32_t> g_chars;
// The name dialog's message: a guest buffer the text lookup returns for kNameTextId, and the
// window's text block it was laid out into (captured in sub_8241B9D0), laid out again as the name
// changes.
uint32_t g_nameText = 0;
std::atomic<uint32_t> g_nameBlock{ 0 }, g_nameFont{ 0 }, g_nameSize{ 0 };
// Message boxes (XamShowMessageBoxUI, kernel_ui.cpp): the queued one, the one on screen, its
// message (kBoxTextId, laid out again into g_boxBlock as the listed choice moves) and - while a
// window is being opened - its button labels for the lookups of the window's OK / CANCEL texts.
constexpr uint32_t kBoxTextId = 0x7F03;
enum class BoxStyle { Information, Confirmation, List };
std::mutex g_boxMutex;
std::vector<SysUiMessageBox> g_boxQueue;
struct BoxState
{
    bool showing = false, releasing = false;
    SysUiMessageBox box;
    BoxStyle style = BoxStyle::Information;
    int cursor = 0;
    uint16_t prev = 0;
} g_box;
std::atomic<bool> g_boxOwns{ false };
uint32_t g_boxText = 0;
std::atomic<uint32_t> g_boxBlock{ 0 }, g_boxFont{ 0 }, g_boxSize{ 0 };
std::mutex g_labelMutex;
std::vector<uint32_t> g_labels;             // guest texts, set only around the window's opening
// The keyboard (XamShowKeyboardUI, kernel_ui.cpp): the queued request, the one on screen, its
// message (kKeyboardTextId, laid out again into g_kbBlock as the text changes) and the characters
// typed since the last frame.
// Text ids the port supplies: 0x7F01 exit, 0x7F02 name dialog, 0x7F03 message box, 0x7F04 switch
// profile (menu_doc.cpp retargets the game's texts to 0x7F01 / 0x7F04), 0x7F05 keyboard.
constexpr uint32_t kKeyboardTextId = 0x7F05;
std::mutex g_kbMutex;
std::vector<SysUiKeyboard> g_kbQueue;
std::vector<uint32_t> g_kbChars;
struct KeyboardState
{
    bool showing = false, releasing = false;
    SysUiKeyboard kb;
    std::string text, note;
    uint16_t prev = 0;
} g_kb;
std::atomic<bool> g_kbOwns{ false };
uint32_t g_kbText = 0;
std::atomic<uint32_t> g_kbBlock{ 0 }, g_kbFont{ 0 }, g_kbSize{ 0 };

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string Two(int i) { return std::string(i < 10 ? "0" : "") + std::to_string(i); }
bool TitleLogo() { return NowMs() - g_titleLogoMs.load() < 150; }

void Notify(uint32_t id, uint32_t param) { XamNotifyEnqueueEvent(id, param); }

void BuildProfiles(Ui& ui)
{
    ui.rows.clear();
    for (const profiles::Profile& p : profiles::List())
        ui.rows.push_back({ p.name, "", "Play as " + p.name + "." });
    if (ui.rows.size() < profiles::kMaxProfiles)
        ui.rows.push_back({ "CREATE NEW PROFILE", "", "Make a new profile with its own save data." });
}

// "PLAYER <n>": the first free number from the profile count up.
std::string SuggestedName()
{
    const std::vector<profiles::Profile> list = profiles::List();
    for (size_t n = list.size() + 1;; ++n)
    {
        const std::string name = "PLAYER " + std::to_string(n);
        bool used = false;
        for (const profiles::Profile& p : list) used |= _stricmp(p.name.c_str(), name.c_str()) == 0;
        if (!used) return name;
    }
}

// ---- the name dialog: the game's confirmation window (OK / CANCEL), answered from the host
std::string NameMessage(const Ui& ui)
{
    std::string hint = ui.note;
    if (hint.empty())
        hint = ui.suggested ? "Type a name, or confirm this one." : "ENTER: confirm   ESC: cancel";
    return "Name your new profile.\n\n" + ui.name + (ui.suggested ? "" : "_") + "\n\n" + hint;
}
// A text the lookup hook returns: a fixed guest buffer (allocated on first use) holding `text` as
// a text-table entry (length byte in front).
void WriteTableText(uint8_t* base, uint32_t& buffer, size_t capacity, const std::string& text)
{
    if (!buffer)
    {
        const std::vector<char> zeros(capacity + 1, 0);
        buffer = GuestCopy(base, zeros.data(), zeros.size(), 4);
    }
    const size_t n = (std::min)(text.size(), capacity);
    std::memcpy(base + buffer, text.data(), n);
    base[buffer + n] = 0;
    GWrite8(base, buffer - 1, static_cast<uint8_t>((std::min<size_t>)(n, 255)));
}
void WriteNameText(uint8_t* base, const std::string& text) { WriteTableText(base, g_nameText, 480, text); }
void ShowNameDialog(PPCContext& ctx, uint8_t* base, const Ui& ui)
{
    WriteNameText(base, NameMessage(ui));
    g_nameBlock = 0;
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = 0;
    ctx.r4.u64 = kNameTextId;
    ctx.r5.u64 = 0;
    sub_8241FAF0(ctx, base);                          // lib::WindowShowConfirmation(text id)
}
void UpdateNameDialog(PPCContext& ctx, uint8_t* base, const Ui& ui)
{
    WriteNameText(base, NameMessage(ui));
    const uint32_t block = g_nameBlock.load();
    if (!block) return;
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = block;
    ctx.r4.u64 = g_nameFont.load();
    ctx.r5.u64 = g_nameSize.load();
    ctx.r6.u64 = g_nameText;
    sub_8241B9D0(ctx, base);                          // the window lays its message out again
}
void HideDialog(PPCContext& ctx, uint8_t* base)
{
    GuestFrame frame(ctx, base, 0x100);
    sub_8241E7C8(ctx, base);                          // lib::WindowHideDialog()
}

// ---- Options -> GRAPHICS SETTINGS (display_settings::Graphics): the page's rows, in order.
enum GraphicsRow { kVsync, kRenderScale, kAntiAliasing, kFiltering, kSharpen, kBrightness, kFps, kDefault, kGraphicsRows };
void BuildGraphics(Ui& ui)
{
    const display_settings::Graphics g = display_settings::GetGraphics();
    static const char* const sharpen[] = { "OFF", "LOW", "MEDIUM", "HIGH" };
    const int aniso = display_settings::Anisotropies()[size_t(g.anisotropy)];
    ui.rows = {
        { "V-SYNC", g.vsync ? "ON" : "OFF", "ON: no tearing. OFF: lowest latency; the picture may tear." },
        { "RENDER SCALE", std::to_string(display_settings::RenderScales()[size_t(g.renderScale)]) + "%",
          "Render above the resolution and scale down for a cleaner image. Demanding." },
        { "ANTI-ALIASING", g.fxaa ? "FXAA" : "OFF", "FXAA smooths jagged edges on the final image." },
        { "TEXTURE FILTERING", aniso > 1 ? std::to_string(aniso) + "X ANISOTROPIC" : "STANDARD",
          "Anisotropic filtering keeps floors and walls sharp at an angle." },
        { "SHARPENING", sharpen[g.sharpen], "Sharpen the final image." },
        { "BRIGHTNESS", std::to_string(g.brightness), "Adjust the brightness of the picture." },
        { "FPS COUNTER", g.fpsCounter ? "ON" : "OFF", "Show frames per second in the corner. The game runs at a fixed 60." },
        { "DEFAULT", "", "Reset to default settings." },
    };
}
// LEFT / RIGHT (dir -1 / +1; A cycles forward) on a row; DEFAULT resets with A.
void ChangeGraphics(int row, int dir)
{
    display_settings::Graphics g = display_settings::GetGraphics();
    auto step = [dir](int v, int count) { return (v + dir + count) % count; };
    switch (row)
    {
    case kVsync: g.vsync = !g.vsync; break;
    case kRenderScale: g.renderScale = step(g.renderScale, int(display_settings::RenderScales().size())); break;
    case kAntiAliasing: g.fxaa = !g.fxaa; break;
    case kFiltering: g.anisotropy = step(g.anisotropy, int(display_settings::Anisotropies().size())); break;
    case kSharpen: g.sharpen = step(g.sharpen, 4); break;
    case kBrightness: g.brightness = std::clamp(g.brightness + dir, 0, 10); break;
    case kFps: g.fpsCounter = !g.fpsCounter; break;
    case kDefault: if (dir > 0) g = {}; break;
    }
    display_settings::SetGraphics(g);
}

// ---- Options -> VOICE CHAT (display_settings::Voice, voice_chat.h): mode, talk key, volume, then
// the players met this session (A / LEFT / RIGHT mutes or unmutes one).
enum VoiceRow { kVoiceMode, kVoiceKey, kVoiceVolume, kVoicePlayers };
std::atomic<bool> g_voiceCapture{ false };               // PUSH-TO-TALK KEY waits for a key
std::atomic<int> g_voiceKey{ 0 };
std::vector<uint64_t> g_voicePeers;                      // the player rows' peers, in row order
void BuildVoice(Ui& ui)
{
    const display_settings::Voice v = display_settings::GetVoice();
    static const char* const modes[] = { "OFF", "OPEN MIC", "PUSH TO TALK" };
    const bool capturing = g_voiceCapture.load();
    ui.rows = {
        { "VOICE CHAT", modes[v.mode], "Talk with players in online lobbies and matches. F8 mutes everyone." },
        { "PUSH-TO-TALK KEY", capturing ? "PRESS A KEY" : keyboard_keys::KeyName(v.pushToTalkKey),
          capturing ? "Press the key to hold while talking.   ESC: cancel" : "A: choose the key you hold to talk (PUSH TO TALK)." },
        { "VOICE VOLUME", std::to_string(v.volume), "How loud other players are." },
    };
    g_voicePeers.clear();
    for (const voice_chat::Player& p : voice_chat::Players())
    {
        g_voicePeers.push_back(p.peer);
        std::string name = p.name;
        for (char& c : name)                                  // what the game's font draws
            if (c < 0x20 || c > 0x7E || c == '~') c = '?';
        if (name.size() > 24) name.resize(24);
        ui.rows.push_back({ name, p.muted ? "MUTED" : p.talking ? "TALKING" : "ON",
                            "A: mute or unmute this player. Your microphone is chosen in Steam's voice settings." });
    }
    if (g_voicePeers.empty())
        ui.rows.push_back({ "NO PLAYERS YET", "", "Players from your online lobbies show here, to mute them." });
}
void ChangeVoice(int row, int dir)
{
    display_settings::Voice v = display_settings::GetVoice();
    if (row == kVoiceMode) v.mode = (v.mode + dir + 3) % 3;
    else if (row == kVoiceVolume) v.volume = std::clamp(v.volume + dir, 0, 10);
    else if (row >= kVoicePlayers && size_t(row - kVoicePlayers) < g_voicePeers.size())
    {
        const uint64_t peer = g_voicePeers[size_t(row - kVoicePlayers)];
        display_settings::SetVoiceMuted(peer, !display_settings::IsVoiceMuted(peer));
        return;
    }
    display_settings::SetVoice(v);
}

// ---- Options -> UPDATES (updater.h): this build's version, the latest release's, CHECK FOR UPDATES
// and DOWNLOAD AND INSTALL. Opening the page checks; each answer is told in a message box (a newer
// release asks to download it; once downloaded, asks to install - the game then closes and starts
// again). Message box answers set g_updateAction, done on the next frame (a box cannot open
// another from its own answer).
enum UpdateRow { kUpdateCurrent, kUpdateLatest, kUpdateCheck, kUpdateInstall };
std::atomic<int> g_updateAction{ 0 };                    // 1 download, 2 install
updater::State g_updateTold = updater::State::Idle;       // the last state a message box reported
void BuildUpdate(Ui& ui)
{
    const updater::Status st = updater::Get();
    using S = updater::State;
    const std::string latest = st.state == S::Checking ? "CHECKING..." : st.latest.empty() ? "-" : st.latest;
    std::string status, install;
    switch (st.state)
    {
    case S::Idle: break;
    case S::Checking: status = "CHECKING..."; break;
    case S::UpToDate: status = "UP TO DATE"; break;
    case S::Available: status = "UPDATE AVAILABLE"; break;
    case S::Failed: status = "FAILED"; break;
    case S::Downloading: status = "UPDATE AVAILABLE"; install = "DOWNLOADING " + std::to_string(st.percent) + "%"; break;
    case S::Downloaded: status = "UPDATE AVAILABLE"; install = "READY"; break;
    }
    std::string checkHelp = "A: check again. Updates come from github.com/" + updater::Repo() + ".";
    if (st.state == S::Failed && !st.error.empty()) checkHelp = "Could not check: " + st.error + ".";
    ui.rows = {
        { "INSTALLED VERSION", updater::CurrentVersion(), "The version you are playing." },
        { "LATEST RELEASE", latest, "The newest version published on GitHub." },
        { "CHECK FOR UPDATES", status, checkHelp },
        { "DOWNLOAD AND INSTALL", install, "A: download the newest version and install it. The game restarts afterwards." },
    };
}
void QuitForUpdate()
{
    std::fprintf(stderr, "[menu] quitting for the update\n");
    discord_presence::Shutdown();
    std::fflush(stderr);
    std::fflush(stdout);
    LogDrain(); TerminateProcess(GetCurrentProcess(), 0);
}
void UpdateBox(const std::string& text, std::vector<std::string> buttons, int action)
{
    SysUiMessageBox box;
    box.text = text;
    box.buttons = std::move(buttons);
    box.cancel = int(box.buttons.size()) - 1;
    box.done = [action](int choice) { if (choice == 0 && action) g_updateAction = action; };
    SysUiShowMessageBox(std::move(box));
}
void ConfirmInstall(const updater::Status& st)
{
    if (!updater::CanInstall())
    {
        UpdateBox("Version " + st.latest + " is downloaded, but this copy of the game\nis not an installed one, so it cannot be updated here.",
                  { "OK" }, 0);
        return;
    }
    UpdateBox("Version " + st.latest + " is ready to install.\n\nThe game closes, Windows asks for permission,\nand the game starts again when the update is done.",
              { "INSTALL", "LATER" }, 2);
}
// Every frame on the page: the actions chosen in message boxes, and a box for each new answer.
void UpdateFrame()
{
    using S = updater::State;
    const int action = g_updateAction.exchange(0);
    if (action == 1) updater::Download();
    if (action == 2)
    {
        std::string error;
        if (updater::Install(error)) { QuitForUpdate(); return; }
        UpdateBox("The update could not be started:\n" + error, { "OK" }, 0);
    }
    const updater::Status st = updater::Get();
    if (st.state == g_updateTold) return;
    bool shown = true;
    switch (st.state)
    {
    case S::UpToDate:
        UpdateBox(std::string("You are already on the latest update.\n\nInstalled: ") + updater::CurrentVersion() +
                  (st.latest.empty() ? "" : "\nLatest release: " + st.latest), { "OK" }, 0);
        break;
    case S::Available:
        UpdateBox("Version " + st.latest + " is available.\nYou have " + updater::CurrentVersion() + ".\n\nDownload and install it now?",
                  { "YES", "NO" }, 1);
        break;
    case S::Failed:
        UpdateBox("The update failed:\n" + st.error + ".", { "OK" }, 0);
        break;
    case S::Downloaded: ConfirmInstall(st); break;
    default: shown = false; break;
    }
    (void)shown;
    g_updateTold = st.state;
}

void Open(Ui& ui, int cursor, Screen screen = Screen::Profiles)
{
    ui.screen = screen;
    ui.cursor = cursor;
    ui.setupFrames = 1;
    ui.shown.clear();
    ui.shownCursor = -1;
    g_textEntry = false;
    g_owns = true;
}
void Close(PPCContext& ctx, uint8_t* base, Ui& ui, bool continueAfter)
{
    if (ui.screen == Screen::Name) HideDialog(ctx, base);
    SwitchFrame(ctx, base, "FrameSysEmpty");
    ui.screen = Screen::None;
    ui.frameRows = 0;
    ui.releasing = true;
    ui.continueAfter = continueAfter;
    g_textEntry = false;
    Notify(9, 0);                                        // XN_SYS_UI closed
}
void SignInAndClose(PPCContext& ctx, uint8_t* base, Ui& ui, int index)
{
    profiles::SignIn(index);
    Notify(0x0000000Au, 1);                              // XN_SYS_SIGNINCHANGED, user 0
    Close(ctx, base, ui, true);                          // and the title takes START
}

void DrawCursor(PPCContext& ctx, uint8_t* base, int count, int cursor, bool force, const std::string& icon);
// The Options room behind the Options pages: each page's OnFocus requests the menu background
// lumen into slot 3 and plays its option_bg label (MEASURED 2026-10-22: lib::MenuLumenRequest(3,
// "lumen_main_menu_bg.lm", "lumen_main_menu_bg.nut", 0, 0, 0), impl sub_8256FDA8, C strings; then
// lib::MenuIFLumenMoviePlayFromLabel("bg_all", "option_bg", false)). The port's own pages have no
// OnFocus, so they do the same when they come up.
void OptionsBackground(PPCContext& ctx, uint8_t* base)
{
    {
        GuestFrame frame(ctx, base, 0x100);
        ctx.r3.u64 = 3;
        ctx.r4.u64 = GuestStr(base, "lumen_main_menu_bg.lm");
        ctx.r5.u64 = GuestStr(base, "lumen_main_menu_bg.nut");
        ctx.r6.u64 = 0; ctx.r7.u64 = 0; ctx.r8.u64 = 0;
        sub_8256FDA8(ctx, base);
    }
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, "bg_all");
    ctx.r4.u64 = GuestStr(base, "option_bg");
    ctx.r5.u64 = 0;
    sub_8256D578(ctx, base);                          // lib::MenuIFLumenMoviePlayFromLabel
}
void Draw(PPCContext& ctx, uint8_t* base, Ui& ui)
{
    if (ui.screen == Screen::Graphics) BuildGraphics(ui);
    else if (ui.screen == Screen::Voice) BuildVoice(ui);
    else if (ui.screen == Screen::Update) BuildUpdate(ui);
    else BuildProfiles(ui);
    const int count = std::clamp(int(ui.rows.size()), 2, kRows);
    ui.rows.resize(size_t(count), Row{ "-", "", "-" });
    ui.cursor = std::clamp(ui.cursor, 0, count - 1);
    if (count != ui.frameRows)
    {
        // The frame with exactly this many rows (its bars follow its icon arrays); write the
        // texts once it is up.
        SwitchFrame(ctx, base, ("FrameSysMenu" + std::to_string(count)).c_str());
        ui.frameRows = count;
        ui.shown.clear();
        ui.shownCursor = -1;
        ui.setupFrames = 2;
        return;
    }
    if (ui.shown.empty())
    {
        // As Display Settings' OnFocus does: no large icon (IFMainMenuL would show its first,
        // the globe), and the bottom bar scrolls the game's tips.
        MenuMovieVisible(ctx, base, "rmc_icon_menu_L", false);
        if (ui.screen != Screen::Profiles) OptionsBackground(ctx, base);
        MenuTelopTarget(ctx, base, "StaticTelopSysMenu" + std::to_string(count));
    }
    const std::string n = std::to_string(count) + "_";
    for (int i = 0; i < count; ++i)
    {
        const Row& row = ui.rows[size_t(i)];
        const bool fresh = i >= int(ui.shown.size());
        const Row old = fresh ? Row{} : ui.shown[size_t(i)];
        const std::string k = n + Two(i + 1);
        if (fresh || old.label != row.label) MenuSetText(ctx, base, "MenuItemSysMenu" + k, row.label);
        if (fresh || old.value != row.value) MenuSetText(ctx, base, "StaticValueSysMenu" + k, row.value);
        if (fresh || old.help != row.help) MenuSetText(ctx, base, "StaticSysMenu" + k, row.help);
    }
    if (ui.shown.empty()) ui.settle = 8;
    ui.shown = ui.rows;
    if (ui.cursor != ui.shownCursor)
    {
        // Select Profile shows the main menu's PROFILE icon, GRAPHICS SETTINGS Display Settings' monitor.
        DrawCursor(ctx, base, count, ui.cursor, ui.shownCursor < 0,
                   ui.screen == Screen::Graphics || ui.screen == Screen::Update ? "icon_s_046"
                   : ui.screen == Screen::Voice                                  ? "icon_s_000"
                                                                                 : "icon_s_006");
        ui.shownCursor = ui.cursor;
    }
}

// The cursor as the interface script global::IFMainMenuL_Cursor / _CursorSet draws it (menus get no
// input on the title, so their OnTrigger never runs): the selection bar, the row icons (the
// selected one lit), and only the selected row's help text.
void DrawCursor(PPCContext& ctx, uint8_t* base, int count, int cursor, bool force, const std::string& icon)
{
    const std::string n = std::to_string(count);
    MenuSetCursor(ctx, base, "MenuSysMenu" + n, cursor);
    MenuMoviePlay(ctx, base, "amc_menu_bar_sentaku_L", "menu_bar_sentaku_solo_" + Two(cursor + 1), force);
    for (int i = 0; i < count; ++i)
    {
        MenuMoviePlay(ctx, base, "mc_icon_menu_S_L_" + Two(i + 1), i == cursor ? icon : icon + "_x", force);
        const std::string help = "StaticSysMenu" + n + "_0" + std::to_string(i + 1);
        MenuTextHide(ctx, base, help, i != cursor);
        MenuTickCount(ctx, base, help, 0);
    }
}

// lib::MenuGetCurrentFrameName.
std::string CurrentFrameName(PPCContext& ctx, uint8_t* base)
{
    GuestFrame frame(ctx, base, 0x100);
    sub_8256C100(ctx, base);
    return GuestString(base, ctx.r3.u32, 64);
}

// Pressed-this-frame buttons, with auto-repeat for the directions (400 ms, then every 90 ms).
uint16_t Pressed(Ui& ui)
{
    const uint16_t now = HostControllerButtons();
    uint16_t pressed = now & ~ui.prev;
    const uint16_t dirs = now & (kUp | kDown | kLeft | kRight);
    const int64_t t = NowMs();
    if (dirs && dirs == ui.repeatKey && t >= ui.repeatAt) { pressed |= dirs; ui.repeatAt = t + 90; }
    else if (dirs != ui.repeatKey) { ui.repeatKey = dirs; ui.repeatAt = t + 400; }
    ui.prev = now;
    return pressed;
}

void OpenNameDialog(PPCContext& ctx, uint8_t* base, Ui& ui)
{
    ui.screen = Screen::Name;
    ui.name = SuggestedName();
    ui.suggested = true;
    ui.note.clear();
    g_textEntry = true;
    ShowNameDialog(ctx, base, ui);
    std::fprintf(stderr, "[sys-ui] new profile: name dialog\n");
}
void CloseNameDialog(PPCContext& ctx, uint8_t* base, Ui& ui)
{
    HideDialog(ctx, base);
    g_textEntry = false;
    ui.screen = Screen::Profiles;
    ui.shownCursor = -1;
}

// ---- message boxes in the game's dialog windows (see SysUiMessageBox)
std::string Upper(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string BoxMessage(const BoxState& b)
{
    if (b.style != BoxStyle::List) return b.box.text;
    std::string m = b.box.text + "\n";
    for (size_t i = 0; i < b.box.buttons.size(); ++i)
    {
        const std::string label = Upper(b.box.buttons[i]);
        m += "\n" + (int(i) == b.cursor ? "> " + label + " <" : label);
    }
    return m;
}
void ShowBox(PPCContext& ctx, uint8_t* base, const BoxState& b)
{
    WriteTableText(base, g_boxText, 2000, BoxMessage(b));
    g_boxBlock = 0;
    {
        static std::map<std::string, uint32_t> pool;
        std::lock_guard l(g_labelMutex);
        g_labels.clear();
        if (b.style != BoxStyle::List)
            for (const std::string& label : b.box.buttons)
            {
                auto it = pool.find(Upper(label));
                if (it == pool.end()) it = pool.emplace(Upper(label), TableText(base, Upper(label).c_str())).first;
                g_labels.push_back(it->second);
            }
    }
    {
        GuestFrame frame(ctx, base, 0x100);
        ctx.r3.u64 = 0;
        ctx.r4.u64 = kBoxTextId;
        ctx.r5.u64 = 1;                               // information: the A button's label
        if (b.style == BoxStyle::Confirmation) sub_8241FAF0(ctx, base);   // lib::WindowShowConfirmation
        else sub_8241FB70(ctx, base);                                    // lib::WindowShowInformation
    }
    std::lock_guard l(g_labelMutex);
    g_labels.clear();
}
void UpdateBox(PPCContext& ctx, uint8_t* base, const BoxState& b)
{
    WriteTableText(base, g_boxText, 2000, BoxMessage(b));
    const uint32_t block = g_boxBlock.load();
    if (!block) return;
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = block;
    ctx.r4.u64 = g_boxFont.load();
    ctx.r5.u64 = g_boxSize.load();
    ctx.r6.u64 = g_boxText;
    sub_8241B9D0(ctx, base);                          // the window lays its message out again
}
void BoxFrame(PPCContext& ctx, uint8_t* base)
{
    BoxState& b = g_box;
    const uint16_t now = HostControllerButtons();
    const uint16_t pressed = now & ~b.prev;
    b.prev = now;
    if (b.releasing)
    {
        if (now == 0) { b.releasing = false; g_boxOwns = false; }   // the answer's press stays ours
        return;
    }
    if (!b.showing)
    {
        {
            std::lock_guard l(g_boxMutex);
            if (g_boxQueue.empty()) return;
            b.box = std::move(g_boxQueue.front());
            g_boxQueue.clear();
        }
        const size_t n = b.box.buttons.size();
        const bool shortLabels = n == 2 && b.box.buttons[0].size() <= 8 && b.box.buttons[1].size() <= 8;
        b.style = n == 1 ? BoxStyle::Information : shortLabels ? BoxStyle::Confirmation : BoxStyle::List;
        b.cursor = std::clamp(b.box.active, 0, int(n) - 1);
        b.showing = true;
        g_boxOwns = true;
        ShowBox(ctx, base, b);
        return;
    }
    int choice = -1;
    switch (b.style)
    {
    case BoxStyle::Information:
        if (pressed & (kA | kB | kStart)) choice = 0;
        break;
    case BoxStyle::Confirmation:
        if (pressed & (kA | kStart)) choice = 0;
        else if (pressed & kB) choice = 1;
        break;
    case BoxStyle::List:
    {
        const int n = int(b.box.buttons.size());
        if (pressed & (kUp | kDown))
        {
            b.cursor = (b.cursor + ((pressed & kUp) ? n - 1 : 1)) % n;
            MenuSound(ctx, base, kSoundCursor);
            UpdateBox(ctx, base, b);
        }
        if (pressed & (kA | kStart)) choice = b.cursor;
        else if ((pressed & kB) && b.box.cancel >= 0) choice = b.box.cancel;
        break;
    }
    }
    if (choice < 0) return;
    MenuSound(ctx, base, choice == b.box.cancel ? kSoundCancel : kSoundDecide);
    HideDialog(ctx, base);
    b.showing = false;
    b.releasing = true;
    std::fprintf(stderr, "[sys-ui] message box answered: %d '%s'\n", choice, b.box.buttons[size_t(choice)].c_str());
    const std::function<void(int)> done = std::move(b.box.done);
    if (done) done(choice);
}

// ---- the keyboard in the game's confirmation window (see SysUiKeyboard)
std::string KeyboardMessage(const KeyboardState& k)
{
    std::string m;
    if (!k.kb.title.empty()) m += k.kb.title + "\n";
    if (!k.kb.description.empty()) m += k.kb.description + "\n";
    m += "\n" + k.text + "_\n\n" + (k.note.empty() ? std::string("ENTER: send   ESC: cancel") : k.note);
    return m;
}
void UpdateKeyboard(PPCContext& ctx, uint8_t* base, const KeyboardState& k)
{
    WriteTableText(base, g_kbText, 2000, KeyboardMessage(k));
    const uint32_t block = g_kbBlock.load();
    if (!block) return;
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = block;
    ctx.r4.u64 = g_kbFont.load();
    ctx.r5.u64 = g_kbSize.load();
    ctx.r6.u64 = g_kbText;
    sub_8241B9D0(ctx, base);                          // the window lays its message out again
}
void KeyboardFrame(PPCContext& ctx, uint8_t* base)
{
    KeyboardState& k = g_kb;
    const uint16_t now = HostControllerButtons();
    const uint16_t pressed = now & ~k.prev;
    k.prev = now;
    if (k.releasing)
    {
        if (now == 0) { k.releasing = false; g_kbOwns = false; }   // the answer's press stays ours
        return;
    }
    if (!k.showing)
    {
        {
            std::lock_guard l(g_kbMutex);
            if (g_kbQueue.empty()) return;
            k.kb = std::move(g_kbQueue.front());
            g_kbQueue.clear();
            g_kbChars.clear();
            g_kbOwns = true;
        }
        // Only what fits and what the game's font draws.
        k.text.clear();
        for (char c : k.kb.text)
            if (c >= 0x20 && c <= 0x7E && k.text.size() < k.kb.maxLength) k.text.push_back(c);
        k.note.clear();
        k.showing = true;
        WriteTableText(base, g_kbText, 2000, KeyboardMessage(k));
        g_kbBlock = 0;
        GuestFrame frame(ctx, base, 0x100);
        ctx.r3.u64 = 0;
        ctx.r4.u64 = kKeyboardTextId;
        ctx.r5.u64 = 0;
        sub_8241FAF0(ctx, base);                      // lib::WindowShowConfirmation(text id)
        std::fprintf(stderr, "[sys-ui] keyboard '%s' (at most %zu characters)\n", k.kb.title.c_str(), k.kb.maxLength);
        return;
    }
    std::vector<uint32_t> chars;
    {
        std::lock_guard l(g_kbMutex);
        chars.swap(g_kbChars);
    }
    int answer = -1;                                  // 1 send, 0 cancel
    bool changed = false;
    for (uint32_t c : chars)
    {
        if (c == '\r') { answer = 1; break; }
        if (c == 0x1B) { answer = 0; break; }
        k.note.clear();
        changed = true;
        if (c == '\b') { if (!k.text.empty()) k.text.pop_back(); }
        else if (c < 0x20 || c > 0x7E) changed = false;
        else if (k.text.size() >= k.kb.maxLength) k.note = "At most " + std::to_string(k.kb.maxLength) + " characters.";
        else k.text.push_back(char(c));
    }
    if (answer < 0 && (pressed & (kA | kStart))) answer = 1;
    else if (answer < 0 && (pressed & kB)) answer = 0;
    if (answer < 0)
    {
        if (changed) UpdateKeyboard(ctx, base, k);
        return;
    }
    MenuSound(ctx, base, answer ? kSoundDecide : kSoundCancel);
    HideDialog(ctx, base);
    k.showing = false;
    k.releasing = true;
    std::fprintf(stderr, "[sys-ui] keyboard %s (%zu characters)\n", answer ? "sent" : "cancelled", k.text.size());
    const auto done = std::move(k.kb.done);
    if (done) done(answer == 1, k.text);
}

// ---- the voice panel: VOICE CHAT's settings in the game's information window, opened with F7 or
// the right stick's click (R3) in the menus or the online lobby - never at character select or in
// a fight. The game's mode manager [0x82F30B94] holds the current mode at +12 (MEASURED
// 2026-10-22: 16 the main menu and its submenus, Options included; 29 Online Mode's menus; 35 the
// online session room;
// 8 character select, 9 a fight). UP / DOWN choose a line, LEFT / RIGHT or A change it (a
// player's line mutes / unmutes), B closes. The lines follow voice_chat as players come and go.
constexpr uint32_t kVoicePanelTextId = 0x7F06;
constexpr uint16_t kRightStick = 0x0080;
struct VoicePanel
{
    bool showing = false, releasing = false;
    int cursor = 0;
    std::vector<uint64_t> peers;                     // the player lines' peers
    std::string shown;                               // the text as last laid out
} g_vp;
std::atomic<bool> g_vpOwns{ false };
uint32_t g_vpText = 0;
std::atomic<uint32_t> g_vpBlock{ 0 }, g_vpFont{ 0 }, g_vpSize{ 0 };

std::string VoicePanelText(VoicePanel& vp)
{
    static const char* const modes[] = { "OFF", "OPEN MIC", "PUSH TO TALK" };
    const display_settings::Voice v = display_settings::GetVoice();
    std::vector<std::string> lines = {
        std::string("VOICE CHAT: ") + modes[v.mode] + (v.mode == display_settings::VoicePushToTalk ? " (" + keyboard_keys::KeyName(v.pushToTalkKey) + ")" : ""),
        "VOICE VOLUME: " + std::to_string(v.volume),
    };
    vp.peers.clear();
    for (const voice_chat::Player& p : voice_chat::Players())
    {
        std::string name = p.name;
        for (char& c : name)
            if (c < 0x20 || c > 0x7E || c == '~') c = '?';
        if (name.size() > 20) name.resize(20);
        lines.push_back(name + ": " + (p.muted ? "MUTED" : p.talking ? "TALKING" : "ON"));
        vp.peers.push_back(p.peer);
    }
    vp.cursor = std::clamp(vp.cursor, 0, int(lines.size()) - 1);
    std::string m = "VOICE CHAT\n";
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (i == 2) m += "\n";
        m += "\n" + (int(i) == vp.cursor ? "> " + lines[i] + " <" : lines[i]);
    }
    if (vp.peers.empty()) m += "\n\nNo players yet.";
    if (voice_chat::AllMuted()) m += "\n\nF8: everyone is muted.";
    m += "\n\nUP / DOWN: choose   LEFT / RIGHT: change   B: close";
    return m;
}
void ShowVoicePanel(PPCContext& ctx, uint8_t* base, VoicePanel& vp)
{
    vp.shown = VoicePanelText(vp);
    WriteTableText(base, g_vpText, 2000, vp.shown);
    g_vpBlock = 0;
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = 0;
    ctx.r4.u64 = kVoicePanelTextId;
    ctx.r5.u64 = 1;
    sub_8241FB70(ctx, base);                          // lib::WindowShowInformation(text id)
}
void UpdateVoicePanel(PPCContext& ctx, uint8_t* base, VoicePanel& vp)
{
    const std::string text = VoicePanelText(vp);
    if (text == vp.shown) return;
    vp.shown = text;
    WriteTableText(base, g_vpText, 2000, text);
    const uint32_t block = g_vpBlock.load();
    if (!block) return;
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = block;
    ctx.r4.u64 = g_vpFont.load();
    ctx.r5.u64 = g_vpSize.load();
    ctx.r6.u64 = g_vpText;
    sub_8241B9D0(ctx, base);                          // the window lays its message out again
}
void VoicePanelFrame(PPCContext& ctx, uint8_t* base)
{
    VoicePanel& vp = g_vp;
    static uint16_t prev = 0;
    static bool prevF7 = false;
    const uint16_t now = HostControllerButtons();
    const uint16_t pressed = now & ~prev;
    prev = now;
    const bool front = keyui_Foreground();
    const bool f7 = front && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool f7Pressed = f7 && !prevF7;
    prevF7 = f7;
    if (vp.releasing)
    {
        if (now == 0) { vp.releasing = false; g_vpOwns = false; }
        return;
    }
    if (!vp.showing)
    {
        if (!(f7Pressed || (pressed & kRightStick))) return;
        // Only in the menus or the session room, with nothing else of ours open.
        if (g_owns.load() || g_boxOwns.load() || g_kbOwns.load()) return;
        const uint32_t manager = GRead32(base, 0x82F30B94);
        const uint32_t mode = manager ? GRead32(base, manager + 12) : 0;
        if (mode != 16 && mode != 29 && mode != 35) return;
        std::fprintf(stderr, "[sys-ui] voice panel (mode %u)\n", mode);
        vp.showing = true;
        g_vpOwns = true;
        MenuSound(ctx, base, kSoundDecide);
        ShowVoicePanel(ctx, base, vp);
        return;
    }
    const int lines = 2 + int(vp.peers.size());
    if (pressed & (kUp | kDown))
    {
        vp.cursor = (vp.cursor + ((pressed & kUp) ? lines - 1 : 1)) % lines;
        MenuSound(ctx, base, kSoundCursor);
    }
    else if (pressed & (kLeft | kRight | kA))
    {
        const int dir = (pressed & kLeft) ? -1 : 1;
        display_settings::Voice v = display_settings::GetVoice();
        if (vp.cursor == 0) { v.mode = (v.mode + dir + 3) % 3; display_settings::SetVoice(v); }
        else if (vp.cursor == 1) { v.volume = std::clamp(v.volume + dir, 0, 10); display_settings::SetVoice(v); }
        else if (size_t(vp.cursor - 2) < vp.peers.size())
        {
            const uint64_t peer = vp.peers[size_t(vp.cursor - 2)];
            display_settings::SetVoiceMuted(peer, !display_settings::IsVoiceMuted(peer));
        }
        MenuSound(ctx, base, kSoundCursor);
    }
    else if ((pressed & (kB | kRightStick)) || f7Pressed)
    {
        MenuSound(ctx, base, kSoundCancel);
        HideDialog(ctx, base);
        vp.showing = false;
        vp.releasing = true;
        return;
    }
    UpdateVoicePanel(ctx, base, vp);
}

// Confirm: an empty name takes the suggestion; the new profile signs in.
void Finish(PPCContext& ctx, uint8_t* base, Ui& ui)
{
    std::string name = profiles::Normalize(ui.name);
    if (name.empty()) name = SuggestedName();
    if (!profiles::ValidName(name)) { ui.note = "Use letters, digits and spaces."; return; }
    const int index = profiles::Create(name);
    if (index < 0) { ui.note = "That name is already used."; return; }
    MenuSound(ctx, base, kSoundDecide);
    SignInAndClose(ctx, base, ui, index);
}

void Frame(PPCContext& ctx, uint8_t* base)
{
    BoxFrame(ctx, base);
    KeyboardFrame(ctx, base);
    VoicePanelFrame(ctx, base);
    Ui& ui = g_ui;
    const uint16_t pressed = Pressed(ui);
    if (ui.releasing)
    {
        if (ui.prev == 0)
        {
            ui.releasing = false;
            g_owns = false;
            if (ui.continueAfter) HostPulseStart();
        }
        return;
    }
    if (g_signInRequest.exchange(false) && !profiles::SignedIn())
    {
        // The game asks itself (XamShowSigninUI): its sign-in states draw nothing, so the last
        // used profile signs in. (The title's START no longer gets there: see TitleLogo.)
        const int index = profiles::LastIndex();
        Notify(9, 1);
        profiles::SignIn(index);
        Notify(0x0000000Au, 1);
        Notify(9, 0);
        std::fprintf(stderr, "[sys-ui] the game asked for a sign-in: last profile\n");
        return;
    }
    if (ui.screen == Screen::None)
    {
        const std::string current = CurrentFrameName(ctx, base);
        if (current == "FrameGraphicsSettings" || current == "FrameVoiceSettings" || current == "FrameUpdates")
        {
            // Options -> GRAPHICS SETTINGS / VOICE CHAT / UPDATES played Options out and switched to
            // its empty frame.
            const Screen screen = current == "FrameGraphicsSettings" ? Screen::Graphics
                                  : current == "FrameVoiceSettings"  ? Screen::Voice
                                                                     : Screen::Update;
            Open(ui, 0, screen);
            if (screen == Screen::Update)
            {
                g_updateTold = updater::State::Checking;         // tell this check's answer
                updater::Check();
            }
            std::fprintf(stderr, "[sys-ui] %s\n", current.c_str());
            return;
        }
        if (g_switchProfile.load() && TitleLogo())
        {
            // SWITCH PROFILE: the game is back on the title; the profile signs out here (not in
            // the middle of the mode change) and Select Profile opens by itself.
            g_switchProfile = false;
            profiles::SignOut();
            Notify(0x0000000Au, 1);                      // XN_SYS_SIGNINCHANGED, user 0
            Open(ui, profiles::LastIndex());
            Notify(9, 1);
            std::fprintf(stderr, "[sys-ui] switch profile: Select Profile\n");
            return;
        }
        if (!profiles::SignedIn() && TitleLogo() && (pressed & (kStart | kA)))
        {
            Open(ui, profiles::LastIndex());
            MenuSound(ctx, base, kSoundDecide);
            Notify(9, 1);                                // XN_SYS_UI opened
            std::fprintf(stderr, "[sys-ui] Select Profile\n");
        }
        return;
    }
    if (ui.setupFrames > 0)
    {
        if (--ui.setupFrames == 0) Draw(ctx, base, ui);
        return;
    }

    std::vector<uint32_t> chars;
    {
        std::lock_guard l(g_charMutex);
        chars.swap(g_chars);
    }
    if (ui.screen == Screen::Voice && g_voiceCapture.load())
    {
        // PUSH-TO-TALK KEY: the next key pressed (ESC, or a pad's B, cancels). The keys held then
        // count as pressed only once they are let go.
        const int vk = g_voiceKey.exchange(0);
        bool done = (pressed & kB) != 0;
        if (vk && vk != VK_ESCAPE && keyboard_keys::Assignable(vk))
        {
            display_settings::Voice v = display_settings::GetVoice();
            v.pushToTalkKey = vk;
            display_settings::SetVoice(v);
            MenuSound(ctx, base, kSoundDecide);
            done = true;
        }
        else if (vk == VK_ESCAPE) done = true;
        if (done)
        {
            g_voiceCapture = false;
            ui.prev = 0xFFFF;
        }
        Draw(ctx, base, ui);
        return;
    }
    if ((ui.screen == Screen::Graphics || ui.screen == Screen::Voice || ui.screen == Screen::Update) && g_boxOwns.load())
    {
        Draw(ctx, base, ui);                                 // a message box has the controller
        return;
    }
    if (ui.screen == Screen::Update) UpdateFrame();
    if (ui.screen == Screen::Update && (pressed & (kA | kStart)) && !(pressed & (kUp | kDown)))
    {
        using S = updater::State;
        const updater::Status st = updater::Get();
        MenuSound(ctx, base, kSoundDecide);
        if (ui.cursor == kUpdateCheck) { g_updateTold = S::Checking; updater::Check(); }
        else if (ui.cursor == kUpdateInstall)
        {
            if (st.state == S::Available || (st.state == S::Failed && !st.latest.empty())) updater::Download();
            else if (st.state == S::Downloaded) ConfirmInstall(st);
            else if (st.state == S::UpToDate) g_updateTold = S::Idle;   // tell it again
            else if (st.state != S::Checking && st.state != S::Downloading) { g_updateTold = S::Checking; updater::Check(); }
        }
        Draw(ctx, base, ui);
        return;
    }
    if (ui.screen == Screen::Graphics || ui.screen == Screen::Voice || ui.screen == Screen::Update)
    {
        const bool graphics = ui.screen == Screen::Graphics;
        if (ui.settle > 0 && --ui.settle == 0) ui.shownCursor = -1;
        const int rows = int(ui.rows.size());
        if (pressed & (kUp | kDown))
        {
            ui.cursor = (ui.cursor + ((pressed & kUp) ? rows - 1 : 1)) % rows;
            MenuSound(ctx, base, kSoundCursor);
        }
        else if ((pressed & (kLeft | kRight)) && ui.screen != Screen::Update && (graphics ? ui.cursor != kDefault : ui.cursor != kVoiceKey))
        {
            if (graphics) ChangeGraphics(ui.cursor, (pressed & kLeft) ? -1 : 1);
            else ChangeVoice(ui.cursor, (pressed & kLeft) ? -1 : 1);
            MenuSound(ctx, base, kSoundCursor);
        }
        else if (pressed & (kA | kStart))
        {
            if (graphics) ChangeGraphics(ui.cursor, 1);
            else if (ui.cursor == kVoiceKey) { g_voiceKey = 0; g_voiceCapture = true; }
            else ChangeVoice(ui.cursor, 1);
            MenuSound(ctx, base, kSoundDecide);
        }
        else if (pressed & kB)
        {
            // Back to Options (its menu kept its cursor on GRAPHICS SETTINGS).
            MenuSound(ctx, base, kSoundCancel);
            SwitchFrame(ctx, base, "FrameMode007-01");
            ui.screen = Screen::None;
            ui.frameRows = 0;
            ui.releasing = true;
            ui.continueAfter = false;
            return;
        }
        Draw(ctx, base, ui);
        return;
    }
    if (ui.screen == Screen::Profiles)
    {
        if (ui.settle > 0 && --ui.settle == 0) ui.shownCursor = -1;   // draw the cursor once more
        const int rows = int(ui.rows.size());
        if (pressed & (kUp | kDown))
        {
            ui.cursor = (ui.cursor + ((pressed & kUp) ? rows - 1 : 1)) % rows;
            MenuSound(ctx, base, kSoundCursor);
        }
        if (pressed & (kA | kStart))
        {
            MenuSound(ctx, base, kSoundDecide);
            if (ui.cursor < int(profiles::List().size())) { SignInAndClose(ctx, base, ui, ui.cursor); return; }
            OpenNameDialog(ctx, base, ui);
            return;
        }
        if (pressed & kB)
        {
            MenuSound(ctx, base, kSoundCancel);
            Close(ctx, base, ui, false);                 // stays on the logo, signed out
            return;
        }
        Draw(ctx, base, ui);
        return;
    }

    // The name dialog: typed text edits the name, ENTER / A / START confirm, ESC / B cancel.
    bool changed = false;
    for (uint32_t c : chars)
    {
        if (c == '\r') { Finish(ctx, base, ui); if (ui.screen != Screen::Name) return; changed = true; continue; }
        if (c == 0x1B) { MenuSound(ctx, base, kSoundCancel); CloseNameDialog(ctx, base, ui); return; }
        const bool printable = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ';
        if (c != '\b' && !printable) continue;
        if (ui.suggested) { ui.name.clear(); ui.suggested = false; }
        ui.note.clear();
        if (c == '\b') { if (!ui.name.empty()) ui.name.pop_back(); }
        else if (ui.name.size() >= profiles::kMaxName) ui.note = "A name has at most 15 characters.";
        else if (c != ' ' || (!ui.name.empty() && ui.name.back() != ' ')) ui.name.push_back(char(c));
        changed = true;
    }
    if (pressed & (kA | kStart)) { Finish(ctx, base, ui); if (ui.screen != Screen::Name) return; changed = true; }
    else if (pressed & kB) { MenuSound(ctx, base, kSoundCancel); CloseNameDialog(ctx, base, ui); return; }
    if (changed) UpdateNameDialog(ctx, base, ui);
}

} // namespace sysui

// ---- Options -> Controller Setup: KEYBOARD mode (KEYBOARD_PLAN.md) ------------------------------
// The screen's artwork has room for three rows, so player 1's BUTTON SETTINGS row carries a mode in
// its value text (menu_doc.cpp): PAD (the game's own button settings) or KEYBOARD, switched with
// LEFT / RIGHT while no other button is held (the game's own change is a held button + LEFT /
// RIGHT). In KEYBOARD mode the button panel's ten slots show each button's key instead of its
// action - the game writes the slots with lib::MenuEntitySetText, so its texts are recorded there
// (the hook below) and put back when the mode or the row is left - and the game sees only UP /
// DOWN on that row. A starts editing in the panel itself: a highlighted slot moves with the
// arrows, LB / RB turn to the second page (UP DOWN LEFT RIGHT START BACK and DEFAULTS), A asks
// for the slot's key (PRESS A KEY; keys then arrive from the window, SysUiOnKey, while the game
// gets neutral input), B or ESC ends editing. No windows open.
namespace keyui {

using namespace keyboard_keys;
constexpr int kButtonSettingsRow = 1;                    // BUTTON SETTINGS in player 1's menu
constexpr uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008, kPadStart = 0x0010,
                   kPadLB = 0x0100, kPadRB = 0x0200, kPadA = 0x1000, kPadB = 0x2000, kDpad = 0x000F;
constexpr int kDefaults = -2, kNone = -1;                 // page 2 cells without a control
const std::string kMenu = "MenuMode007-03-01-1", kValue = "StaticTextMenuItemMode007-03-02-1",
                  kHelp = "StaticTextMode007-03-02-1";
// Panel slot StaticTextMode007-03-SS-1-1 (SS = 01..10): the left column top down, then the right
// (MEASURED on screen 2026-10-14). Slot = column * 5 + row.
const int kPage[2][10] = {
    { LB, LT, X, A, LS, RB, RT, Y, B, RS },
    { Up, Down, Left, Right, kNone, Start, Back, kNone, kNone, kDefaults },
};

struct State
{
    bool here = false;                                   // Controller Setup was up last frame
    bool keyboard = false;                               // BUTTON SETTINGS shows KEYBOARD
    bool editing = false, capturing = false, releasing = false;
    int page = 0, slot = 0;
    std::string value;                                   // the value text as last written
    std::string gameText[10];                            // the game's slot texts (icon + action)
    std::string shown[10];                               // the slots while keys are shown
    bool showing = false;
    std::string help;                                    // the help line while keys are shown
    uint16_t prevButtons = 0;
    bool prevEsc = false;
} g_state;
std::atomic<bool> g_capture{ false };
std::atomic<uint16_t> g_gameMask{ 0xFFFF };              // the buttons the game may see (XamInputGetState)
std::mutex g_keyMutex;
std::vector<int> g_keys;                                 // keys pressed since the last frame

int SlotOf(const std::string& widget)
{
    // "StaticTextMode007-03-SS-1-1"
    static const std::string prefix = "StaticTextMode007-03-", suffix = "-1-1";
    if (widget.size() != prefix.size() + 2 + suffix.size() || widget.compare(0, prefix.size(), prefix) != 0 ||
        widget.compare(prefix.size() + 2, suffix.size(), suffix) != 0)
        return -1;
    const int s = (widget[prefix.size()] - '0') * 10 + (widget[prefix.size() + 1] - '0');
    return s >= 1 && s <= 10 ? s - 1 : -1;
}
std::string SlotWidget(int slot) { return "StaticTextMode007-03-" + sysui::Two(slot + 1) + "-1-1"; }

// A key's name as short as the panel's slots need.
std::string ShortKeyName(int vk)
{
    switch (vk)
    {
    case VK_BACK: return "BKSP";
    case VK_PRIOR: return "PG UP";
    case VK_NEXT: return "PG DN";
    default: return KeyName(vk);
    }
}

// What a slot shows: on page 1 the game's icon part ("~white~<icon> ") and the key, on page 2 the
// control's name and its key (just PRESS A KEY while it waits: the help line names the control).
// The key is in the panel's colour (~default~), the slot being edited in yellow - the colours the
// game's text system knows: red green yellow white gray blue lightgray default; a text has to
// start with one (~white~, as the game's own) for the others to be read, and two tags must not
// touch ("~~" breaks them: the whole text then shows literally).
std::string SlotText(const State& st, int slot)
{
    const int control = kPage[st.page][slot];
    if (control == kNone) return "";
    const bool selected = st.editing && slot == st.slot;
    const std::string colour = selected ? "~yellow~" : "~default~";
    if (control == kDefaults) return "~white~ " + colour + "DEFAULTS";
    const bool asking = st.capturing && selected;
    const std::string key = asking ? std::string("PRESS A KEY") : ShortKeyName(Key(control));
    if (st.page == 1)
        return asking ? "~white~ " + colour + key : std::string("~white~") + ControlName(control) + " " + colour + key;
    const std::string& game = st.gameText[slot];
    const size_t at = game.find("~default~");
    return (at == std::string::npos ? std::string("~white~") : game.substr(0, at)) + colour + key;
}

std::string HelpText(const State& st)
{
    const int control = kPage[st.page][st.slot];
    if (st.capturing) return std::string("Press a key for ") + ControlName(control) + ".   ESC: cancel";
    if (st.editing && control == kDefaults) return "A: restore the default keys   B: done";
    if (st.editing) return "A: change   LB / RB: page   B: done";
    return "A: edit keys";
}

std::string CurrentFrame(PPCContext& ctx, uint8_t* base)
{
    GuestFrame frame(ctx, base, 0x100);
    sub_8256C100(ctx, base);                              // lib::MenuGetCurrentFrameName
    return GuestString(base, ctx.r3.u32, 64);
}
int MenuCursor(PPCContext& ctx, uint8_t* base, const std::string& menu)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = GuestStr(base, menu);
    sub_8256C968(ctx, base);                              // lib::MenuContainerGetCursor(menu)
    return static_cast<int32_t>(ctx.r3.u32);
}
// The game's own help text of BUTTON SETTINGS (textid360 4,635), back after the keyboard hints.
void RestoreHelp(PPCContext& ctx, uint8_t* base)
{
    GuestFrame frame(ctx, base, 0x100);
    ctx.r3.u64 = 4;
    ctx.r4.u64 = 635;
    ctx.r5.u64 = GuestStr(base, kHelp);
    sub_8256C7A8(ctx, base);                              // lib::MenuEntitySetTextID(table, id, widget)
}

bool Foreground()
{
    DWORD process = 0;
    const HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &process);
    return w && process == GetCurrentProcessId();
}
bool AnyKeyDown()
{
    for (int vk = 8; vk < 256; ++vk)
        if (GetAsyncKeyState(vk) & 0x8000) return true;
    return false;
}

// The next slot from `slot` in a direction (page 2 skips its empty cells; rows and columns wrap).
int Move(int page, int slot, uint16_t dir)
{
    int col = slot / 5, row = slot % 5;
    for (int tries = 0; tries < 10; ++tries)
    {
        if (dir == kPadUp) row = (row + 4) % 5;
        else if (dir == kPadDown) row = (row + 1) % 5;
        else col ^= 1;
        if (kPage[page][col * 5 + row] != kNone) return col * 5 + row;
        if (dir & (kPadLeft | kPadRight)) dir = kPadUp;   // an empty cell across: go up to a used one
    }
    return slot;
}

void Edit(PPCContext& ctx, uint8_t* base, State& st, uint16_t pressed, bool esc)
{
    if (st.capturing)
    {
        std::vector<int> keys;
        {
            std::lock_guard l(g_keyMutex);
            keys.swap(g_keys);
        }
        bool done = (pressed & kPadB) != 0;               // a controller's B cancels too
        for (int vk : keys)
        {
            if (done) break;
            if (vk == VK_ESCAPE) { done = true; break; }
            if (!Assignable(vk)) continue;
            Assign(kPage[st.page][st.slot], vk);
            MenuSound(ctx, base, kSoundDecide);
            done = true;
        }
        if (!done) return;
        st.capturing = false;
        st.releasing = true;                              // g_capture stays until the keys are up
        return;
    }
    if (pressed & (kPadB) || esc)
    {
        st.editing = false;
        MenuSound(ctx, base, kSoundCancel);
        return;
    }
    if (pressed & (kPadLB | kPadRB))
    {
        st.page ^= 1;
        st.slot = 0;
        MenuSound(ctx, base, kSoundCursor);
        return;
    }
    for (const uint16_t dir : { kPadUp, kPadDown, kPadLeft, kPadRight })
        if (pressed & dir)
        {
            st.slot = Move(st.page, st.slot, dir);
            MenuSound(ctx, base, kSoundCursor);
            return;
        }
    if (pressed & (kPadA | kPadStart))
    {
        const int control = kPage[st.page][st.slot];
        MenuSound(ctx, base, kSoundDecide);
        if (control == kDefaults) { RestoreDefaults(); return; }
        st.capturing = true;
        {
            std::lock_guard l(g_keyMutex);
            g_keys.clear();
        }
        g_capture = true;
    }
}

void Draw(PPCContext& ctx, uint8_t* base, State& st, bool keys)
{
    if (keys)
    {
        for (int s = 0; s < 10; ++s)
        {
            const std::string text = SlotText(st, s);
            if (!st.showing || st.shown[s] != text) MenuSetText(ctx, base, SlotWidget(s), text);
            st.shown[s] = text;
        }
        const std::string help = HelpText(st);
        if (!st.showing || help != st.help) MenuSetText(ctx, base, kHelp, help);
        st.help = help;
        st.showing = true;
    }
    else if (st.showing)
    {
        for (int s = 0; s < 10; ++s)
            if (!st.gameText[s].empty()) MenuSetText(ctx, base, SlotWidget(s), st.gameText[s]);
        RestoreHelp(ctx, base);
        st.showing = false;
    }
}

void Frame(PPCContext& ctx, uint8_t* base)
{
    State& st = g_state;
    if (CurrentFrame(ctx, base) != "FrameControlerSetup")
    {
        if (st.here) std::fprintf(stderr, "[keyboard] controller setup closed\n");
        State fresh;                                      // the game's slot texts stay recorded
        std::copy(std::begin(st.gameText), std::end(st.gameText), std::begin(fresh.gameText));
        st = fresh;
        g_capture = false;
        g_gameMask = 0xFFFF;
        return;
    }
    const uint16_t buttons = HostControllerButtons();
    const bool esc = Foreground() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    if (!st.here)
    {
        st.here = true;                                   // every visit starts on PAD
        st.prevButtons = buttons;                         // the A that opened the screen is not ours
        st.prevEsc = esc;
    }
    const uint16_t pressed = buttons & ~st.prevButtons;
    const bool escPressed = esc && !st.prevEsc;
    st.prevButtons = buttons;
    st.prevEsc = esc;

    if (st.releasing)
    {
        // The new key is still down: it acts as its button only once released.
        if (!AnyKeyDown() && !(buttons & 0xF3FF)) { st.releasing = false; g_capture = false; st.prevButtons = 0; }
    }
    else if (st.editing) Edit(ctx, base, st, pressed, escPressed);
    else if (MenuCursor(ctx, base, kMenu) == kButtonSettingsRow)
    {
        // Not while a button or trigger is held: that is the game's own change of its action.
        if ((pressed & (kPadLeft | kPadRight)) && !(buttons & ~kDpad) && !HostTriggerHeld())
        {
            st.keyboard = !st.keyboard;
            MenuSound(ctx, base, kSoundCursor);
        }
        else if (st.keyboard && (pressed & kPadA))
        {
            st.editing = true;
            st.page = 0;
            st.slot = 0;
            MenuSound(ctx, base, kSoundDecide);
        }
    }
    const bool onRow = st.editing || MenuCursor(ctx, base, kMenu) == kButtonSettingsRow;
    g_gameMask = st.editing ? uint16_t(0) : (onRow && st.keyboard) ? uint16_t(0x0003) : uint16_t(0xFFFF);

    const std::string value = st.keyboard ? "KEYBOARD" : "PAD";
    if (value != st.value)
    {
        MenuSetText(ctx, base, kValue, value);
        st.value = value;
    }
    Draw(ctx, base, st, onRow && st.keyboard);
}

} // namespace keyui

// Runs on the game's main thread once per frame (end of VdSwap, registers restored after): the
// system UI, and a diagnostics trigger - C:/ttt2/port/ui.trigger holding a frame name switches
// the menu to that frame.
void UiFrame(PPCContext& ctx, uint8_t* base)
{
    sysui::Frame(ctx, base);
    keyui::Frame(ctx, base);
    static uint32_t tick = 0;
    if (++tick % 30) return;
    FILE* f = std::fopen("C:/ttt2/port/ui.trigger", "rb");
    if (!f) return;
    char name[128] = {};
    const size_t n = std::fread(name, 1, sizeof(name) - 1, f);
    std::fclose(f);
    DeleteFileA("C:/ttt2/port/ui.trigger");
    name[n] = 0;
    for (char* p = name; *p; ++p) if (*p == '\r' || *p == '\n') { *p = 0; break; }
    std::fprintf(stderr, "[menu-ext] ui trigger: '%s'\n", name);
    if (*name) SwitchFrame(ctx, base, name);
}
struct UiHookInstall { UiHookInstall() { g_uiFrameHook = UiFrame; } } g_uiHookInstall;
} // namespace

bool SysUiRequestSignIn() { sysui::g_signInRequest = true; return true; }
bool SysUiOwnsInput()
{
    return sysui::g_owns.load() || sysui::g_boxOwns.load() || sysui::g_kbOwns.load() || sysui::g_vpOwns.load() ||
           keyui::g_capture.load();
}
bool SysUiKeyCaptureActive() { return keyui::g_capture.load() || sysui::g_voiceCapture.load(); }
uint16_t SysUiGameButtonMask() { return keyui::g_gameMask.load(); }
void SysUiOnKey(int vk)
{
    if (sysui::g_voiceCapture.load()) { sysui::g_voiceKey = vk; return; }
    if (!keyui::g_capture.load()) return;
    std::lock_guard l(keyui::g_keyMutex);
    if (keyui::g_keys.size() < 16) keyui::g_keys.push_back(vk);
}
bool SysUiTitleWantsProfile() { return !profiles::SignedIn() && sysui::TitleLogo(); }
bool SysUiTextEntryActive() { return sysui::g_textEntry.load() || sysui::g_kbOwns.load(); }
static bool KeyboardBusy()
{
    std::lock_guard l(sysui::g_kbMutex);
    return sysui::g_kbOwns.load() || !sysui::g_kbQueue.empty();
}
bool SysUiShowMessageBox(SysUiMessageBox box)
{
    if (KeyboardBusy()) return false;                 // one dialog window at a time
    std::lock_guard l(sysui::g_boxMutex);
    if (sysui::g_boxOwns.load() || !sysui::g_boxQueue.empty() || box.buttons.empty()) return false;
    sysui::g_boxQueue.push_back(std::move(box));
    return true;
}
bool SysUiShowKeyboard(SysUiKeyboard keyboard)
{
    {
        std::lock_guard l(sysui::g_boxMutex);
        if (sysui::g_boxOwns.load() || !sysui::g_boxQueue.empty()) return false;
    }
    std::lock_guard l(sysui::g_kbMutex);
    if (sysui::g_kbOwns.load() || !sysui::g_kbQueue.empty()) return false;
    sysui::g_kbQueue.push_back(std::move(keyboard));
    return true;
}
void SysUiOnChar(uint32_t ch)
{
    if (sysui::g_kbOwns.load())
    {
        std::lock_guard l(sysui::g_kbMutex);
        if (sysui::g_kbChars.size() < 64) sysui::g_kbChars.push_back(ch);
        return;
    }
    if (!sysui::g_textEntry.load()) return;
    std::lock_guard l(sysui::g_charMutex);
    if (sysui::g_chars.size() < 64) sysui::g_chars.push_back(ch);
}

namespace {

menu_doc::PcLists PcLists()
{
    menu_doc::PcLists lists;
    lists.modes = { "WINDOWED", "BORDERLESS", "FULLSCREEN" };
    for (const display_settings::Resolution& r : display_settings::Resolutions())
        lists.resolutions.push_back(std::to_string(r.width) + "x" + std::to_string(r.height));
    return lists;
}

// ---- natives -------------------------------------------------------------------------------
// Signature descriptors of the game's own natives (port/tools/xtal_natives.py). Implementations
// use the plain ABI (r3 in, r3 out).
constexpr uint32_t kSetterInt = 0x8203624C;   // void f(int)
constexpr uint32_t kGetterInt = 0x8205B9A4;   // int f()
constexpr uint32_t kGetterBool = 0x820025B8;  // bool f()

PPC_FUNC(NativeSetWindowMode) { (void)base; display_settings::SetMode(static_cast<int>(ctx.r3.u32)); }
PPC_FUNC(NativeGetWindowMode) { (void)base; ctx.r3.u64 = static_cast<uint32_t>(display_settings::GetMode()); }
PPC_FUNC(NativeSetResolution) { (void)base; display_settings::SetResolutionIndex(static_cast<int>(ctx.r3.u32)); }
PPC_FUNC(NativeGetResolution) { (void)base; ctx.r3.u64 = static_cast<uint32_t>(display_settings::GetResolutionIndex()); }
PPC_FUNC(NativeTrue) { (void)base; ctx.r3.u64 = 1; }

// lib::ExitGameWindow - the exit dialog's lib::WindowGetValue: -1 while it waits, 0 for the
// confirm button (A, YES), otherwise NO. Settings are saved as they change; the guest threads
// are not unwound (TerminateProcess, as for a closed window), so nothing can hang on the way out.
PPC_FUNC(NativeExitGameWindow)
{
    __imp__sub_8241E7A8(ctx, base);
    if (static_cast<int32_t>(ctx.r3.u32) == 0)
    {
        std::fprintf(stderr, "[menu] exit confirmed - quitting\n");
        discord_presence::Shutdown();
        std::fflush(stderr);
        std::fflush(stdout);
        LogDrain(); TerminateProcess(GetCurrentProcess(), 0);
    }
}

// lib::SwitchProfileWindow - the SWITCH PROFILE dialog's lib::WindowGetValue: 0 (YES) arms the
// sign-out for when the title is back (the script then requests the SIGNOUT mode).
PPC_FUNC(NativeSwitchProfileWindow)
{
    __imp__sub_8241E7A8(ctx, base);
    if (static_cast<int32_t>(ctx.r3.u32) == 0 && !sysui::g_switchProfile.exchange(true))
        std::fprintf(stderr, "[menu] switch profile confirmed\n");
}

struct HostNative { const char* name; uint32_t signature; PPCFunc* fn; };
const HostNative kNatives[] = {
    // main menu: the 9th item (STORE in the game, EXIT here) is shown when this is true
    { "IsExitMenuItemVisible", kGetterBool, NativeTrue },
    // the PC rows' OnTrigger is 3D DEPTH's with renamed natives: the capability gate, the
    // preview call and the store call (both apply live)
    { "DisplaySettingIsPCDisplayCapable", kGetterBool, NativeTrue },
    { "MenuOptionDisplayWindowMode", kSetterInt, NativeSetWindowMode },
    { "DisplaySettingSetWinMode", kSetterInt, NativeSetWindowMode },
    { "MenuOptionDisplayResolution", kSetterInt, NativeSetResolution },
    { "DisplaySettingSetDispRes", kSetterInt, NativeSetResolution },
    { "DisplaySettingGetWindowMode", kGetterInt, NativeGetWindowMode },
    { "DisplaySettingGetResolution", kGetterInt, NativeGetResolution },
    // the exit confirmation's answer
    { "ExitGameWindow", kGetterInt, NativeExitGameWindow },
    // the switch-profile confirmation's answer
    { "SwitchProfileWindow", kGetterInt, NativeSwitchProfileWindow },
};
// Guest code addresses routed to the host natives: the interior of the development menu loader
// sub_823450B0 (never called in retail; nothing indirect-calls into a function's middle).
constexpr uint32_t kNativeSlotBase = 0x823450C0;

// Define the natives in `lib` exactly as sub_823756A8 defines the game's own:
// sub_82A802F0(&native, signature, &impl), sub_82A6F5E8(&id, name, length, 0),
// sub_82A69330(&copy, &native), sub_822501A0(lib, &id, &copy), sub_82A69360(&native).
void RegisterNatives(PPCContext& ctx, uint8_t* base, uint32_t lib)
{
    GuestFrame frame(ctx, base, 0x200);
    const uint32_t sp = frame.sp();
    const uint32_t implCell = sp + 0x80, native = sp + 0xA0, id = sp + 0xB0, copy = sp + 0xC0;
    for (size_t i = 0; i < sizeof(kNatives) / sizeof(kNatives[0]); ++i)
    {
        const HostNative& n = kNatives[i];
        const uint32_t slot = kNativeSlotBase + uint32_t(i) * 4;
        PPC_LOOKUP_FUNC(base, slot) = n.fn;
        GWrite32(base, implCell, slot);
        ctx.r3.u64 = native; ctx.r4.u64 = n.signature; ctx.r5.u64 = implCell;
        sub_82A802F0(ctx, base);
        ctx.r3.u64 = id; ctx.r4.u64 = GuestCString(base, n.name); ctx.r5.u64 = std::strlen(n.name); ctx.r6.u64 = 0;
        sub_82A6F5E8(ctx, base);
        ctx.r3.u64 = copy; ctx.r4.u64 = native;
        sub_82A69330(ctx, base);
        ctx.r5.u64 = ctx.r3.u32; ctx.r3.u64 = lib; ctx.r4.u64 = id;
        sub_822501A0(ctx, base);
        ctx.r3.u64 = native;
        sub_82A69360(ctx, base);
    }
    std::fprintf(stderr, "[menu] registered %zu PC natives\n", sizeof(kNatives) / sizeof(kNatives[0]));
}

// The exit and switch-profile confirmations (lib::WindowShowConfirmation(id) in the cloned
// dialogs). The window looks up its button labels right after its message: text 0x3D8 "OK" and
// 0x3DC "CANCEL" (MEASURED 2026-10-12); for these dialogs they read YES / NO.
constexpr uint32_t kExitTextId = 0x7F01, kSwitchTextId = 0x7F04, kOkTextId = 0x03D8, kCancelTextId = 0x03DC;
// The lookup below answers the first id that matches: each text the port supplies needs its own.
static_assert(sysui::kNameTextId != kExitTextId && sysui::kNameTextId != kSwitchTextId &&
              sysui::kBoxTextId != kExitTextId && sysui::kBoxTextId != kSwitchTextId &&
              sysui::kKeyboardTextId != kExitTextId && sysui::kKeyboardTextId != kSwitchTextId &&
              sysui::kKeyboardTextId != sysui::kNameTextId && sysui::kKeyboardTextId != sysui::kBoxTextId,
              "port text ids must be distinct");
std::atomic<int> g_exitLabelsPending{ 0 };
} // namespace

// ---- hooks ---------------------------------------------------------------------------------

// Options natives registration; the PC natives are added right after, into the same `lib`:
// sub_823756A8 takes it as [sub_82582860(sub_82254450()) + 56] (script environment singleton).
PPC_FUNC(sub_823756A8)
{
    __imp__sub_823756A8(ctx, base);
    const PPCContext saved = ctx;
    {
        GuestFrame frame(ctx, base, 0x100);
        sub_82254450(ctx, base);
        sub_82582860(ctx, base);
    }
    const uint32_t lib = GRead32(base, ctx.r3.u32 + 56);
    RegisterNatives(ctx, base, lib);
    ctx = saved;
}

// lib::MenuSelectValueSetCursor(cursor, name): when Display Settings initialises MOTION BLUR's
// value (on open and on DEFAULT), the PC rows get their current values (cursor at widget+368); when
// Game Options initialises BATTLE POSITION's, INPUT DELAY gets its own (OFF = 0, ON = 1).
PPC_FUNC(sub_8256CD28)
{
    const uint32_t nameAddr = ctx.r4.u32;
    __imp__sub_8256CD28(ctx, base);
    const std::string name = GuestString(base, nameAddr, 64);
    const bool display = name == "SelectValueMenuItemMode007-04-02", game = name == "SelectValueMenuItemMode007-01-05";
    if (!display && !game) return;
    const PPCContext saved = ctx;
    {
        GuestFrame frame(ctx, base, 0x100);
        static const uint32_t modeName = GuestCString(base, "SelectValueMenuItemMode007-04-06");
        static const uint32_t resName = GuestCString(base, "SelectValueMenuItemMode007-04-07");
        static const uint32_t delayName = GuestCString(base, "SelectValueMenuItemMode007-01-07");
        if (display)
        {
            if (const uint32_t w = FindWidget(ctx, base, modeName)) GWrite32(base, w + 368, uint32_t(display_settings::GetMode()));
            if (const uint32_t w = FindWidget(ctx, base, resName))
                GWrite32(base, w + 368, uint32_t(display_settings::GetResolutionIndex()));
        }
        else if (const uint32_t w = FindWidget(ctx, base, delayName))
            GWrite32(base, w + 368, display_settings::GetInputDelayOn() ? 1u : 0u);
    }
    ctx = saved;
}

// lib::GameOptionSetBattlePosition (impl sub_82374700): Game Options stores its rows as it closes,
// BATTLE POSITION last - INPUT DELAY's cursor is stored with it.
PPC_FUNC(sub_82374700)
{
    __imp__sub_82374700(ctx, base);
    const PPCContext saved = ctx;
    {
        GuestFrame frame(ctx, base, 0x100);
        static const uint32_t delayName = GuestCString(base, "SelectValueMenuItemMode007-01-07");
        if (const uint32_t w = FindWidget(ctx, base, delayName))
            display_settings::SetInputDelayOn(GRead32(base, w + 368) == 1);
    }
    ctx = saved;
}

// lib::GameOptionResetToDefaults (impl sub_823747C0): Game Options' DEFAULT resets INPUT DELAY too
// (its cursor follows when the page re-initialises BATTLE POSITION's).
PPC_FUNC(sub_823747C0)
{
    __imp__sub_823747C0(ctx, base);
    display_settings::SetInputDelayOn(false);
}

// TitleMode::Impl::state_title_main: the logo waiting for START (sub_82622B70); impl+40 counts the
// frames in the state and the attract demo starts when it reaches impl+128 (MEASURED 2026-10-12).
// The logo stays up while the system UI is open.
PPC_FUNC(sub_82623858)
{
    sysui::g_titleLogoMs = sysui::NowMs();
    if (SysUiOwnsInput())
    {
        GWrite32(base, ctx.r3.u32 + 40, 0);
        GWrite32(base, ctx.r3.u32 + 44, 0);
    }
    __imp__sub_82623858(ctx, base);
}

// A dialog window's text block takes a text (block, font, size, text: copied and laid out). The
// name dialog's and the message box's blocks are remembered to lay their texts out again.
PPC_FUNC(sub_8241B9D0)
{
    if (ctx.r6.u32 && ctx.r6.u32 == sysui::g_nameText)
    {
        sysui::g_nameBlock = ctx.r3.u32;
        sysui::g_nameFont = ctx.r4.u32;
        sysui::g_nameSize = ctx.r5.u32;
    }
    else if (ctx.r6.u32 && ctx.r6.u32 == sysui::g_kbText)
    {
        sysui::g_kbBlock = ctx.r3.u32;
        sysui::g_kbFont = ctx.r4.u32;
        sysui::g_kbSize = ctx.r5.u32;
    }
    else if (ctx.r6.u32 && ctx.r6.u32 == sysui::g_vpText)
    {
        sysui::g_vpBlock = ctx.r3.u32;
        sysui::g_vpFont = ctx.r4.u32;
        sysui::g_vpSize = ctx.r5.u32;
    }
    else if (ctx.r6.u32 && ctx.r6.u32 == sysui::g_boxText)
    {
        sysui::g_boxBlock = ctx.r3.u32;
        sysui::g_boxFont = ctx.r4.u32;
        sysui::g_boxSize = ctx.r5.u32;
    }
    __imp__sub_8241B9D0(ctx, base);
}

// lib::MenuEntitySetText(text, widget): Controller Setup's button panel (player 1) - the game's
// texts are recorded, and while the KEYBOARD mode shows keys the game's writes keep the key.
PPC_FUNC(sub_8256C708)
{
    if (!g_hostText && ctx.r4.u32 && ctx.r3.u32)
    {
        const int slot = keyui::SlotOf(GuestString(base, ctx.r4.u32, 40));
        if (slot >= 0)
        {
            keyui::g_state.gameText[slot] = GuestString(base, ctx.r3.u32, 96);
            if (keyui::g_state.showing) ctx.r3.u64 = GuestStr(base, keyui::SlotText(keyui::g_state, slot));
        }
    }
    __imp__sub_8256C708(ctx, base);
}

// Text lookup (text manager, id): the exit confirmation's message and button labels, the name
// dialog's message.
PPC_FUNC(sub_8237F5D0)
{
    const uint32_t id = ctx.r4.u32 & 0xFFFF;
    if (id == kExitTextId || id == kSwitchTextId)
    {
        static const uint32_t exitText = TableText(base, "Exit the game?");
        static const uint32_t switchText = TableText(base, "Switch to another profile?\nThe game returns to the title screen.");
        ctx.r3.u64 = id == kExitTextId ? exitText : switchText;
        g_exitLabelsPending = 2;
        return;
    }
    if ((id == kOkTextId || id == kCancelTextId) && g_exitLabelsPending.load() > 0)
    {
        static const uint32_t yes = TableText(base, "YES"), no = TableText(base, "NO");
        g_exitLabelsPending.fetch_sub(1);
        ctx.r3.u64 = id == kOkTextId ? yes : no;
        return;
    }
    if (id == sysui::kNameTextId && sysui::g_nameText)
    {
        ctx.r3.u64 = sysui::g_nameText;
        return;
    }
    if (id == sysui::kVoicePanelTextId && sysui::g_vpText)
    {
        ctx.r3.u64 = sysui::g_vpText;
        return;
    }
    if (id == sysui::kKeyboardTextId && sysui::g_kbText)
    {
        ctx.r3.u64 = sysui::g_kbText;
        return;
    }
    if (id == sysui::kBoxTextId && sysui::g_boxText)
    {
        ctx.r3.u64 = sysui::g_boxText;
        return;
    }
    if (id == kOkTextId || id == kCancelTextId)
    {
        std::lock_guard l(sysui::g_labelMutex);           // a message box's button labels
        const size_t slot = id == kOkTextId ? 0 : 1;
        if (slot < sysui::g_labels.size())
        {
            ctx.r3.u64 = sysui::g_labels[slot];
            return;
        }
    }
    const uint32_t lr = uint32_t(ctx.lr);
    const bool corner = (id == 0x09 || id == 0x0A || id == 0x0B) && lr >= 0x82277000 && lr < 0x82278000;
    const bool logo = id == 0x05 && lr >= 0x82646000 && lr < 0x82647000;
    if ((corner || logo) && SysUiOwnsInput())
    {
        // The title's PRESS START / FREE PLAY texts (the arcade credit display in the corners,
        // looked up from 0x82277664 / 0x82277818, and the logo's from 0x8264656C - MEASURED
        // 2026-10-12) would sit on the system UI's key hints; they are blank while it is open.
        static const uint32_t empty = TableText(base, "");
        ctx.r3.u64 = empty;
        return;
    }
    __imp__sub_8237F5D0(ctx, base);
    static const bool trace = std::getenv("TTT2_TEXT_TRACE") != nullptr;
    if (trace)
    {
        static std::mutex m;
        static std::map<uint32_t, int> seen;
        std::lock_guard l(m);
        if (seen[id]++ == 0)
            std::fprintf(stderr, "[text] id %04X lr %08X '%s'\n", id, lr, GuestString(base, ctx.r3.u32, 48).c_str());
    }
}

// Retail menu document loader: r3 menu manager, r4 BX01 bytes, r5 size, r6 root widget. The
// Options / main-menu document gets the PC edits; the edited copy lives in a never-freed guest
// allocation, as the reader keeps pointing into its bytes.
PPC_FUNC(sub_82346060)
{
    const uint8_t* doc = base + ctx.r4.u32;
    const uint32_t size = ctx.r5.u32;
    if (MenuOverride())
    {
        char path[MAX_PATH];
        std::snprintf(path, sizeof(path), "C:/ttt2/port/menu_override/%08X.bin", Fnv1a(doc, size));
        if (FILE* f = std::fopen(path, "rb"))
        {
            std::string bytes;
            char chunk[65536];
            for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) bytes.append(chunk, n);
            std::fclose(f);
            ctx.r4.u64 = GuestCopy(base, bytes.data(), bytes.size());
            ctx.r5.u64 = bytes.size();
            std::fprintf(stderr, "[menu] document replaced by %s (%zu bytes)\n", path, bytes.size());
        }
    }
    else
    {
        std::string why, note;
        const std::string edited = menu_doc::ApplyPcEdits(doc, size, PcLists(), &why, &note);
        if (!note.empty()) std::fprintf(stderr, "[menu] %s\n", note.c_str());
        if (!edited.empty())
        {
            if (MenuDump())
            {
                CreateDirectoryA("C:/ttt2/port/menu_bin", nullptr);
                char path[MAX_PATH];
                std::snprintf(path, sizeof(path), "C:/ttt2/port/menu_bin/edited_%08X.bin", Fnv1a(doc, size));
                SaveFile(path, edited.data(), edited.size());
            }
            ctx.r4.u64 = GuestCopy(base, edited.data(), edited.size());
            ctx.r5.u64 = edited.size();
            std::fprintf(stderr, "[menu] Options / main menu: PC rows and EXIT added (%u -> %zu bytes)\n", size, edited.size());
        }
        else if (!why.empty())
            std::fprintf(stderr, "[menu] Options / main menu left unchanged: %s\n", why.c_str());
    }
    __imp__sub_82346060(ctx, base);
}

// BX01 document reader: r3 document, r4 bytes, r5 size. TTT2_MENU_DUMP=1 saves each document.
PPC_FUNC(sub_82A8C170)
{
    if (MenuDump() && ctx.r4.u32 && ctx.r5.u32 && ctx.r5.u32 < (64u << 20))
    {
        static std::atomic<uint32_t> index{ 0 };
        CreateDirectoryA("C:/ttt2/port/menu_bin", nullptr);
        char path[MAX_PATH];
        std::snprintf(path, sizeof(path), "C:/ttt2/port/menu_bin/%03u_%08X.bin", index.fetch_add(1),
                      Fnv1a(base + ctx.r4.u32, ctx.r5.u32));
        SaveFile(path, base + ctx.r4.u32, ctx.r5.u32);
    }
    __imp__sub_82A8C170(ctx, base);
}


// The key info bar (sub_8241B588: window, key glyph, text, -1, 0 - one entry per call, every frame,
// from lib::WindowPrintKeyInfo). In the online session room (mode 35, see the voice panel) the
// voice panel's button follows MENU DISPLAY (the Y glyph 0xE026): the right stick glyph 0x25C6
// (RS, or R3 in the PlayStation styles); with keyboard prompts the red one 0x25C7, which
// button_prompts.cpp draws as RS's key there (the plain stick keeps meaning movement)
// (MEASURED 2026-10-22: the room's bar is START GAME CHAT, BACK TOGGLE LOG DISPLAY, Y MENU
// DISPLAY, A OK, B CLOSE / LEAVE).
extern "C" PPC_FUNC(__imp__sub_8241B588);
PPC_FUNC(sub_8241B588)
{
    const uint32_t window = ctx.r3.u32, key = ctx.r4.u32 & 0xFFFF, r6 = ctx.r6.u32, r7 = ctx.r7.u32;
    __imp__sub_8241B588(ctx, base);
    if (key != 0xE026) return;
    const uint32_t manager = GRead32(base, 0x82F30B94);
    if (!manager || GRead32(base, manager + 12) != 35) return;
    static const uint32_t text = TableText(base, "VOICE CHAT");
    const PPCContext saved = ctx;
    {
        GuestFrame frame(ctx, base, 0x100);
        ctx.r3.u64 = window;
        ctx.r4.u64 = button_prompts::Current() == button_prompts::Style::Keyboard ? 0x25C7 : 0x25C6;
        ctx.r5.u64 = text;
        ctx.r6.u64 = r6;
        ctx.r7.u64 = r7;
        __imp__sub_8241B588(ctx, base);
    }
    ctx = saved;
}
