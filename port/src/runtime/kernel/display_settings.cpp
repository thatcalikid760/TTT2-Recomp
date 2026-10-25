// PC display settings (see display_settings.h). Stored as
//   [Display]
//   Mode=0|1|2            (windowed, borderless, fullscreen)
//   Resolution=1920x1080
//   Sharpness=0..10       (0 = off)
//   AntiAliasing=0|1      (off, FXAA)
//   TextureFilter=0..4    (the game's own, anisotropic 2x / 4x / 8x / 16x)
//   VSync=1|0             (frames at the display's refresh / at once)
//   Shadows=1|0           (cast shadows drawn / not)
//   [Game]
//   InputDelay=0|1        (the game's 2-frame offline input delay: removed / original)
//   KeyboardPlayer=0|1    (the keyboard: part of player 1's controller / a player of its own)
//   [Graphics]
//   VSync=0|1  RenderScale=100|125|150|200  Upscaler=0|1  FsrMode=100|77|67|59|50
//   AntiAliasing=0|1|2 (FXAA=0|1 before it)  Anisotropy=1|2|4|8|16  Sharpen=0..3
//   Shadows=0|1  Brightness=0..10  FpsCounter=0|1
//   [Online]
//   Netcode=0..3          (STANDARD, PRIORITIZE RESPONSE, PRIORITIZE GRAPHICS, CUSTOM)
//   CustomDelay=1..6      (CUSTOM's input delay, frames)
//   CustomRollback=1..8   (CUSTOM's prediction limit, frames)
// in %LOCALAPPDATA%\TTT2Recomp\settings.ini, read once at first use.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "display_settings.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace display_settings {
namespace {

std::mutex g_mutex;
bool g_loaded = false;
int g_mode = Windowed;
int g_resolution = 0;
bool g_inputDelay = false;
Graphics g_graphics;
Voice g_voice;
std::vector<uint64_t> g_muted;
int g_netcode = NetStandard;
int g_customDelay = 1;
int g_customRollback = kMaxRollback;
// The image options as the renderer asks for them every frame and every draw (GetSharpness ..
// GetShadows): lock-free copies of g_graphics, refreshed by MirrorLocked.
std::atomic<int> g_sharpness{ 0 }, g_antiAliasing{ 0 }, g_textureFilter{ 0 }, g_vsync{ 1 }, g_shadows{ 1 };
// KEYBOARD PLAYER, read by the input poll thread every millisecond: lock-free too.
std::atomic<int> g_keyboardPlayer{ 0 };
std::atomic<bool> g_loadedFlag{ false };

void MirrorLocked()
{
    static const int kSharpness[4] = { 0, 3, 6, 10 };   // SHARPENING OFF / LOW / MEDIUM / HIGH on the 0..10 scale
    const Graphics& g = g_graphics;
    g_sharpness = kSharpness[g.sharpen];
    g_antiAliasing = g.antiAliasing == 1 ? 1 : 0;       // FXAA; SMAA is the presenter's own
    g_textureFilter = g.anisotropy;                     // the same steps: off, 2x .. 16x
    g_vsync = g.vsync ? 1 : 0;
    g_shadows = g.shadows ? 1 : 0;
}
std::atomic<uint32_t> g_generation{ 0 };

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

// The index of `value` in `list`, or 0.
int IndexOf(const std::vector<int>& list, int value)
{
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i] == value) return int(i);
    return 0;
}

Graphics Clamped(Graphics g)
{
    g.renderScale = std::clamp(g.renderScale, 0, int(RenderScales().size()) - 1);
    g.anisotropy = std::clamp(g.anisotropy, 0, int(Anisotropies().size()) - 1);
    g.upscaler = std::clamp(g.upscaler, 0, 1);
    g.fsrMode = std::clamp(g.fsrMode, 0, int(FsrModes().size()) - 1);
    g.antiAliasing = std::clamp(g.antiAliasing, 0, 2);
    g.sharpen = std::clamp(g.sharpen, 0, 3);
    g.brightness = std::clamp(g.brightness, 0, 10);
    return g;
}

void LoadLocked()
{
    if (g_loaded) return;
    g_loaded = true;
    const std::string ini = IniPath();
    g_mode = std::clamp<int>(GetPrivateProfileIntA("Display", "Mode", Windowed, ini.c_str()), Windowed, Fullscreen);
    char res[32] = {};
    GetPrivateProfileStringA("Display", "Resolution", "1280x720", res, sizeof(res), ini.c_str());
    unsigned w = 0, h = 0;
    g_resolution = 2;                                   // 1280x720 unless the file names another
    if (std::sscanf(res, "%ux%u", &w, &h) == 2)
    {
        const auto& list = Resolutions();
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].width == w && list[i].height == h) g_resolution = int(i);
    }
    g_inputDelay = GetPrivateProfileIntA("Game", "InputDelay", 0, ini.c_str()) != 0;
    g_keyboardPlayer = GetPrivateProfileIntA("Game", "KeyboardPlayer", 0, ini.c_str()) != 0 ? 1 : 0;
    const Graphics d;
    Graphics& g = g_graphics;
    g.vsync = GetPrivateProfileIntA("Graphics", "VSync", d.vsync, ini.c_str()) != 0;
    g.renderScale = IndexOf(RenderScales(), GetPrivateProfileIntA("Graphics", "RenderScale", 100, ini.c_str()));
    g.upscaler = GetPrivateProfileIntA("Graphics", "Upscaler", d.upscaler, ini.c_str());
    g.fsrMode = IndexOf(FsrModes(), GetPrivateProfileIntA("Graphics", "FsrMode", 100, ini.c_str()));
    g.antiAliasing = GetPrivateProfileIntA("Graphics", "AntiAliasing",
                                           GetPrivateProfileIntA("Graphics", "FXAA", d.antiAliasing, ini.c_str()), ini.c_str());
    g.anisotropy = IndexOf(Anisotropies(), GetPrivateProfileIntA("Graphics", "Anisotropy", 1, ini.c_str()));
    g.sharpen = GetPrivateProfileIntA("Graphics", "Sharpen", d.sharpen, ini.c_str());
    g.shadows = GetPrivateProfileIntA("Graphics", "Shadows", d.shadows, ini.c_str()) != 0;
    g.brightness = GetPrivateProfileIntA("Graphics", "Brightness", d.brightness, ini.c_str());
    g.fpsCounter = GetPrivateProfileIntA("Graphics", "FpsCounter", d.fpsCounter, ini.c_str()) != 0;
    g = Clamped(g);
    g_voice.mode = std::clamp<int>(GetPrivateProfileIntA("Voice", "Mode", VoiceOpenMic, ini.c_str()), VoiceOff, VoicePushToTalk);
    g_voice.pushToTalkKey = std::clamp<int>(GetPrivateProfileIntA("Voice", "PushToTalkKey", 'V', ini.c_str()), 1, 255);
    g_voice.volume = std::clamp<int>(GetPrivateProfileIntA("Voice", "Volume", 8, ini.c_str()), 0, 10);
    {
        char muted[2048] = {};
        GetPrivateProfileStringA("Voice", "Muted", "", muted, sizeof(muted), ini.c_str());
        for (char* p = muted; *p;)
        {
            char* end = nullptr;
            const unsigned long long id = std::strtoull(p, &end, 16);
            if (end == p) break;
            if (id) g_muted.push_back(id);
            p = *end ? end + 1 : end;
        }
    }
    g_netcode = std::clamp<int>(GetPrivateProfileIntA("Online", "Netcode", NetStandard, ini.c_str()), NetStandard, NetCustom);
    g_customDelay = std::clamp<int>(GetPrivateProfileIntA("Online", "CustomDelay", 1, ini.c_str()), kMinDelay, kMaxCustomDelay);
    g_customRollback = std::clamp<int>(GetPrivateProfileIntA("Online", "CustomRollback", kMaxRollback, ini.c_str()), 1, kMaxRollback);
    MirrorLocked();
    g_loadedFlag.store(true, std::memory_order_release);
    std::fprintf(stderr, "[display] settings: mode=%d resolution=%ux%u input delay %s, online netcode %d (custom: delay %d, rollback %d) (%s)\n",
                 g_mode, Resolutions()[g_resolution].width, Resolutions()[g_resolution].height, g_inputDelay ? "on" : "off",
                 g_netcode, g_customDelay, g_customRollback, ini.c_str());
}

void SaveLocked()
{
    const std::string ini = IniPath();
    char value[32];
    std::snprintf(value, sizeof(value), "%d", g_mode);
    WritePrivateProfileStringA("Display", "Mode", value, ini.c_str());
    const Resolution r = Resolutions()[g_resolution];
    std::snprintf(value, sizeof(value), "%ux%u", r.width, r.height);
    WritePrivateProfileStringA("Display", "Resolution", value, ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_sharpness.load());
    WritePrivateProfileStringA("Display", "Sharpness", value, ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_antiAliasing.load());
    WritePrivateProfileStringA("Display", "AntiAliasing", value, ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_textureFilter.load());
    WritePrivateProfileStringA("Display", "TextureFilter", value, ini.c_str());
    WritePrivateProfileStringA("Display", "VSync", g_vsync.load() ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("Display", "Shadows", g_shadows.load() ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("Game", "InputDelay", g_inputDelay ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA("Game", "KeyboardPlayer", g_keyboardPlayer.load() ? "1" : "0", ini.c_str());
    const Graphics& g = g_graphics;
    auto put = [&](const char* key, int v) {
        std::snprintf(value, sizeof(value), "%d", v);
        WritePrivateProfileStringA("Graphics", key, value, ini.c_str());
    };
    put("VSync", g.vsync);
    put("RenderScale", RenderScales()[size_t(g.renderScale)]);
    put("Upscaler", g.upscaler);
    put("FsrMode", FsrModes()[size_t(g.fsrMode)]);
    put("AntiAliasing", g.antiAliasing);
    WritePrivateProfileStringA("Graphics", "FXAA", nullptr, ini.c_str());
    put("Anisotropy", Anisotropies()[size_t(g.anisotropy)]);
    put("Sharpen", g.sharpen);
    WritePrivateProfileStringA("Graphics", "MotionBlur", nullptr, ini.c_str());   // 0.2.0 only: Display Settings has the game's own
    put("Shadows", g.shadows);
    WritePrivateProfileStringA("Graphics", "OnlineCrowds", nullptr, ini.c_str()); // 0.2.4 only: crashed online stage loads
    put("Brightness", g.brightness);
    put("FpsCounter", g.fpsCounter);
    auto putVoice = [&](const char* key, int v) {
        std::snprintf(value, sizeof(value), "%d", v);
        WritePrivateProfileStringA("Voice", key, value, ini.c_str());
    };
    putVoice("Mode", g_voice.mode);
    putVoice("PushToTalkKey", g_voice.pushToTalkKey);
    putVoice("Volume", g_voice.volume);
    std::string muted;
    for (uint64_t id : g_muted)
    {
        char hex[24];
        std::snprintf(hex, sizeof(hex), "%s%016llX", muted.empty() ? "" : ",", (unsigned long long)id);
        muted += hex;
    }
    WritePrivateProfileStringA("Voice", "Muted", muted.c_str(), ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_netcode);
    WritePrivateProfileStringA("Online", "Netcode", value, ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_customDelay);
    WritePrivateProfileStringA("Online", "CustomDelay", value, ini.c_str());
    std::snprintf(value, sizeof(value), "%d", g_customRollback);
    WritePrivateProfileStringA("Online", "CustomRollback", value, ini.c_str());
}

// One clamped int setting: stored and logged when it changes.
void SetInt(int& slot, int v, int lo, int hi, const char* what)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    v = std::clamp(v, lo, hi);
    if (v == slot) return;
    slot = v;
    SaveLocked();
    std::fprintf(stderr, "[display] %s -> %d\n", what, v);
}

// The same for the image options (atomics: the renderer reads them without the lock).
void SetAtomic(std::atomic<int>& slot, int v, int lo, int hi, const char* what)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    v = std::clamp(v, lo, hi);
    if (v == slot.load()) return;
    slot.store(v);
    SaveLocked();
    std::fprintf(stderr, "[display] %s -> %d\n", what, v);
}
int GetAtomic(const std::atomic<int>& slot)
{
    if (!g_loadedFlag.load(std::memory_order_acquire)) { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); }
    return slot.load(std::memory_order_relaxed);
}

} // namespace

const std::vector<Resolution>& Resolutions()
{
    // 640x360 and 960x540 render below the console's 720p (half / three quarters of it): for weak
    // GPUs (integrated graphics, GTX 1050 class at high window sizes). The setting is stored as WxH,
    // so adding entries keeps everyone's choice.
    static const std::vector<Resolution> list = {
        { 640, 360 }, { 960, 540 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
    return list;
}

const std::vector<int>& RenderScales()
{
    static const std::vector<int> list = { 100, 125, 150, 200 };
    return list;
}

const std::vector<int>& Anisotropies()
{
    static const std::vector<int> list = { 1, 2, 4, 8, 16 };
    return list;
}

const std::vector<int>& FsrModes()
{
    static const std::vector<int> list = { 100, 77, 67, 59, 50 };
    return list;
}

// LOW / MEDIUM let FSR 1 render below RESOLUTION; HIGH / ULTRA aim for the sharpest picture.
// Columns: render scale, upscaler, FSR mode, anti-aliasing, anisotropy, sharpen (indices).
static const int kPresetValues[kPresets][6] = {
    { 0, 1, 4, 1, 1, 1 },     // LOW:    100%, FSR 1 PERFORMANCE, FXAA, 2x, LOW
    { 0, 1, 2, 1, 3, 1 },     // MEDIUM: 100%, FSR 1 QUALITY, FXAA, 8x, LOW
    { 0, 1, 0, 2, 4, 2 },     // HIGH:   100%, FSR 1 NATIVE, SMAA, 16x, MEDIUM
    { 3, 1, 0, 2, 4, 1 },     // ULTRA:  200%, FSR 1 NATIVE, SMAA, 16x, LOW
};

Graphics Preset(int preset, Graphics g)
{
    const int* v = kPresetValues[std::clamp(preset, 0, kPresets - 1)];
    g.renderScale = v[0]; g.upscaler = v[1]; g.fsrMode = v[2]; g.antiAliasing = v[3]; g.anisotropy = v[4]; g.sharpen = v[5];
    return g;
}

int PresetOf(const Graphics& g)
{
    for (int p = 0; p < kPresets; ++p)
    {
        const int* v = kPresetValues[p];
        if (g.renderScale == v[0] && g.upscaler == v[1] && g.fsrMode == v[2] && g.antiAliasing == v[3] &&
            g.anisotropy == v[4] && g.sharpen == v[5])
            return p;
    }
    return -1;
}

int GetMode() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_mode; }
int GetResolutionIndex() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_resolution; }
Resolution GetResolution() { return Resolutions()[GetResolutionIndex()]; }
uint32_t Generation() { return g_generation.load(); }

void SetMode(int mode)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    mode = std::clamp(mode, int(Windowed), int(Fullscreen));
    if (mode == g_mode) return;
    g_mode = mode;
    SaveLocked();
    g_generation.fetch_add(1);
    std::fprintf(stderr, "[display] mode -> %d\n", mode);
}

void SetResolutionIndex(int index)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    index = std::clamp(index, 0, int(Resolutions().size()) - 1);
    if (index == g_resolution) return;
    g_resolution = index;
    SaveLocked();
    g_generation.fetch_add(1);
    std::fprintf(stderr, "[display] resolution -> %ux%u\n", Resolutions()[index].width, Resolutions()[index].height);
}

bool GetInputDelayOn() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_inputDelay; }

void SetInputDelayOn(bool on)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    if (on == g_inputDelay) return;
    g_inputDelay = on;
    SaveLocked();
    std::fprintf(stderr, "[display] input delay -> %s\n", on ? "on (the original 2 frames offline)" : "off");
}

Graphics GetGraphics() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_graphics; }

void SetGraphics(const Graphics& wanted)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    const Graphics g = Clamped(wanted);
    const Graphics& o = g_graphics;
    if (g.vsync == o.vsync && g.renderScale == o.renderScale && g.upscaler == o.upscaler && g.fsrMode == o.fsrMode &&
        g.antiAliasing == o.antiAliasing && g.anisotropy == o.anisotropy && g.sharpen == o.sharpen &&
        g.shadows == o.shadows && g.brightness == o.brightness && g.fpsCounter == o.fpsCounter)
        return;
    g_graphics = g;
    MirrorLocked();
    SaveLocked();
    g_generation.fetch_add(1);
    std::fprintf(stderr, "[display] graphics: vsync %d, render scale %d%%, upscaler %d (fsr %d%%), aa %d, anisotropy %dx, sharpen %d, shadows %d, brightness %d, fps %d\n",
                 g.vsync, RenderScales()[size_t(g.renderScale)], g.upscaler, FsrModes()[size_t(g.fsrMode)], g.antiAliasing,
                 Anisotropies()[size_t(g.anisotropy)], g.sharpen, g.shadows,
                 g.brightness, g.fpsCounter);
}

Voice GetVoice() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_voice; }

void SetVoice(const Voice& wanted)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    Voice v = wanted;
    v.mode = std::clamp(v.mode, int(VoiceOff), int(VoicePushToTalk));
    v.pushToTalkKey = std::clamp(v.pushToTalkKey, 1, 255);
    v.volume = std::clamp(v.volume, 0, 10);
    if (v.mode == g_voice.mode && v.pushToTalkKey == g_voice.pushToTalkKey && v.volume == g_voice.volume) return;
    g_voice = v;
    SaveLocked();
    std::fprintf(stderr, "[display] voice: mode %d, push-to-talk key %02X, volume %d\n", v.mode, v.pushToTalkKey, v.volume);
}

bool IsVoiceMuted(uint64_t peer)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    return std::find(g_muted.begin(), g_muted.end(), peer) != g_muted.end();
}

void SetVoiceMuted(uint64_t peer, bool muted)
{
    std::lock_guard<std::mutex> l(g_mutex);
    LoadLocked();
    const auto it = std::find(g_muted.begin(), g_muted.end(), peer);
    if (muted == (it != g_muted.end())) return;
    if (muted) g_muted.push_back(peer);
    else g_muted.erase(it);
    SaveLocked();
    std::fprintf(stderr, "[display] voice: %016llX %s\n", (unsigned long long)peer, muted ? "muted" : "unmuted");
}

int GetNetcode() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_netcode; }
int GetCustomDelay() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_customDelay; }
int GetCustomRollback() { std::lock_guard<std::mutex> l(g_mutex); LoadLocked(); return g_customRollback; }
void SetNetcode(int netcode) { SetInt(g_netcode, netcode, NetStandard, NetCustom, "online netcode"); }
void SetCustomDelay(int frames) { SetInt(g_customDelay, frames, kMinDelay, kMaxCustomDelay, "custom input delay"); }
void SetCustomRollback(int frames) { SetInt(g_customRollback, frames, 1, kMaxRollback, "custom rollback"); }

// Upstream's Display Settings setters, onto GRAPHICS SETTINGS (its rows are not added; kept for the natives).
int GetSharpness() { return GetAtomic(g_sharpness); }
int GetAntiAliasing() { return GetAtomic(g_antiAliasing); }
int GetTextureFilter() { return GetAtomic(g_textureFilter); }
bool GetVSync() { return GetAtomic(g_vsync) != 0; }
bool GetShadows() { return GetAtomic(g_shadows) != 0; }
void SetSharpness(int level) { Graphics g = GetGraphics(); g.sharpen = std::clamp(level, 0, kMaxSharpness) * 3 / kMaxSharpness; SetGraphics(g); }
void SetAntiAliasing(int mode) { Graphics g = GetGraphics(); g.antiAliasing = mode ? 1 : 0; SetGraphics(g); }
void SetTextureFilter(int mode) { Graphics g = GetGraphics(); g.anisotropy = std::clamp(mode, 0, kMaxTextureFilter); SetGraphics(g); }
bool GetKeyboardPlayer() { return GetAtomic(g_keyboardPlayer) != 0; }
void SetKeyboardPlayer(bool separate) { SetAtomic(g_keyboardPlayer, separate ? 1 : 0, 0, 1, "keyboard player (1 = separate)"); }
void SetVSync(bool on) { Graphics g = GetGraphics(); g.vsync = on; SetGraphics(g); }
void SetShadows(bool on) { Graphics g = GetGraphics(); g.shadows = on; SetGraphics(g); }

} // namespace display_settings
