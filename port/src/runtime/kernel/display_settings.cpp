// PC display settings (see display_settings.h). Stored as
//   [Display]
//   Mode=0|1|2            (windowed, borderless, fullscreen)
//   Resolution=1920x1080
//   [Game]
//   InputDelay=0|1        (the game's 2-frame offline input delay: removed / original)
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

void LoadLocked()
{
    if (g_loaded) return;
    g_loaded = true;
    const std::string ini = IniPath();
    g_mode = std::clamp<int>(GetPrivateProfileIntA("Display", "Mode", Windowed, ini.c_str()), Windowed, Fullscreen);
    char res[32] = {};
    GetPrivateProfileStringA("Display", "Resolution", "1280x720", res, sizeof(res), ini.c_str());
    unsigned w = 0, h = 0;
    g_resolution = 0;
    if (std::sscanf(res, "%ux%u", &w, &h) == 2)
    {
        const auto& list = Resolutions();
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].width == w && list[i].height == h) g_resolution = int(i);
    }
    g_inputDelay = GetPrivateProfileIntA("Game", "InputDelay", 0, ini.c_str()) != 0;
    std::fprintf(stderr, "[display] settings: mode=%d resolution=%ux%u input delay %s (%s)\n", g_mode,
                 Resolutions()[g_resolution].width, Resolutions()[g_resolution].height, g_inputDelay ? "on" : "off", ini.c_str());
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
    WritePrivateProfileStringA("Game", "InputDelay", g_inputDelay ? "1" : "0", ini.c_str());
}

} // namespace

const std::vector<Resolution>& Resolutions()
{
    static const std::vector<Resolution> list = {
        { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
    return list;
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

} // namespace display_settings
