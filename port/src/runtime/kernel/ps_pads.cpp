// Native DualShock 4 / DualSense input through SDL3 (see ps_pads.h).
#include "ps_pads.h"
#include <cstdio>
#include <cstdlib>
#include <string>

#if TTT2_HAVE_SDL
#include <SDL3/SDL.h>
#endif

namespace ps_pads {
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

// [Input] PlayStationNative, on by default. Written out when absent so the switch is easy to find.
bool Enabled()
{
    const std::string ini = IniPath();
    char value[8] = {};
    GetPrivateProfileStringA("Input", "PlayStationNative", "", value, sizeof(value), ini.c_str());
    if (!*value)
    {
        WritePrivateProfileStringA("Input", "PlayStationNative", "1", ini.c_str());
        return true;
    }
    return std::strtol(value, nullptr, 10) != 0;
}

#if TTT2_HAVE_SDL
static_assert(SDL_GAMEPAD_BUTTON_SOUTH == South && SDL_GAMEPAD_BUTTON_EAST == East &&
              SDL_GAMEPAD_BUTTON_WEST == West && SDL_GAMEPAD_BUTTON_NORTH == North &&
              SDL_GAMEPAD_BUTTON_BACK == Back && SDL_GAMEPAD_BUTTON_GUIDE == Guide &&
              SDL_GAMEPAD_BUTTON_START == Start && SDL_GAMEPAD_BUTTON_LEFT_STICK == LeftStick &&
              SDL_GAMEPAD_BUTTON_RIGHT_STICK == RightStick &&
              SDL_GAMEPAD_BUTTON_LEFT_SHOULDER == LeftShoulder &&
              SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER == RightShoulder &&
              SDL_GAMEPAD_BUTTON_DPAD_UP == DpadUp && SDL_GAMEPAD_BUTTON_DPAD_DOWN == DpadDown &&
              SDL_GAMEPAD_BUTTON_DPAD_LEFT == DpadLeft && SDL_GAMEPAD_BUTTON_DPAD_RIGHT == DpadRight &&
              SDL_GAMEPAD_BUTTON_TOUCHPAD == Touchpad, "SDL gamepad button numbering");
static_assert(SDL_GAMEPAD_AXIS_LEFTX == LeftX && SDL_GAMEPAD_AXIS_LEFTY == LeftY &&
              SDL_GAMEPAD_AXIS_RIGHTX == RightX && SDL_GAMEPAD_AXIS_RIGHTY == RightY &&
              SDL_GAMEPAD_AXIS_LEFT_TRIGGER == LeftTrigger &&
              SDL_GAMEPAD_AXIS_RIGHT_TRIGGER == RightTrigger, "SDL gamepad axis numbering");

struct Slot
{
    SDL_Gamepad* pad = nullptr;
    SDL_JoystickID id = 0;
    bool ps5 = false;
    std::string serial;          // the last pad in this slot, kept after it disconnects
};
Slot g_slots[Slots];
enum class State { Unstarted, Running, Off } g_state = State::Unstarted;
uint64_t g_nextScanMs = 0;

bool StartSdl()
{
    if (!Enabled())
    {
        std::fprintf(stderr, "[input] native PlayStation input off ([Input] PlayStationNative=0)\n");
        return false;
    }
    // Delay-loaded: never call into SDL unless the DLL is really there.
    if (!LoadLibraryW(L"SDL3.dll"))
    {
        std::fprintf(stderr, "[input] SDL3.dll not found - native PlayStation input unavailable\n");
        return false;
    }
    // Only the HIDAPI DualShock 4 / DualSense drivers: every other backend (XInput, RawInput,
    // Windows.Gaming.Input, GameInput, DirectInput) and HIDAPI driver would also see Xbox pads,
    // which XInput already reads, or other devices the game never had.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMEINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
    // XInput pads work with the window in the background; so do these.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD))
    {
        std::fprintf(stderr, "[input] SDL_Init failed (%s) - native PlayStation input unavailable\n", SDL_GetError());
        return false;
    }
    // Nobody drains SDL's event queue: Poll updates the pads itself.
    SDL_SetJoystickEventsEnabled(false);
    SDL_SetGamepadEventsEnabled(false);
    std::fprintf(stderr, "[input] native PlayStation input on (SDL %d.%d.%d)\n", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
                 SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()));
    return true;
}

bool IsPlayStation(SDL_GamepadType type)
{
    return type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
}

// Open new DualShock 4 / DualSense pads: the slot this pad had before, else a slot nobody has
// used, else any free slot.
void Scan()
{
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) return;
    for (int i = 0; i < count; ++i)
    {
        const SDL_JoystickID id = ids[i];
        bool open = false;
        for (const Slot& s : g_slots) open |= s.pad && s.id == id;
        if (open || !IsPlayStation(SDL_GetGamepadTypeForID(id))) continue;
        SDL_Gamepad* pad = SDL_OpenGamepad(id);
        if (!pad) continue;
        const char* serialText = SDL_GetGamepadSerial(pad);
        const std::string serial = serialText ? serialText : "";
        int slot = -1;
        for (int k = 0; k < Slots && slot < 0; ++k)
            if (!g_slots[k].pad && !serial.empty() && g_slots[k].serial == serial) slot = k;
        for (int k = 0; k < Slots && slot < 0; ++k)
            if (!g_slots[k].pad && g_slots[k].serial.empty()) slot = k;
        for (int k = 0; k < Slots && slot < 0; ++k)
            if (!g_slots[k].pad) slot = k;
        if (slot < 0) { SDL_CloseGamepad(pad); continue; }
        g_slots[slot] = { pad, id, SDL_GetGamepadTypeForID(id) == SDL_GAMEPAD_TYPE_PS5, serial };
        const SDL_JoystickConnectionState link = SDL_GetGamepadConnectionState(pad);
        const char* name = SDL_GetGamepadName(pad);
        std::fprintf(stderr, "[input] PlayStation pad connected: %s (%s), host pad %d\n", name ? name : "?",
                     link == SDL_JOYSTICK_CONNECTION_WIRELESS ? "Bluetooth" :
                     link == SDL_JOYSTICK_CONNECTION_WIRED ? "USB" : "unknown link", Slots + slot);
    }
    SDL_free(ids);
}
#endif

} // namespace

void Poll(Pad out[Slots])
{
    for (int k = 0; k < Slots; ++k) out[k] = {};
#if TTT2_HAVE_SDL
    if (g_state == State::Unstarted) g_state = StartSdl() ? State::Running : State::Off;
    if (g_state != State::Running) return;
    SDL_UpdateGamepads();
    const uint64_t now = GetTickCount64();
    if (now >= g_nextScanMs)
    {
        Scan();
        g_nextScanMs = now + 250;   // a plugged-in pad is picked up within a quarter second
    }
    for (int k = 0; k < Slots; ++k)
    {
        Slot& s = g_slots[k];
        if (!s.pad) continue;
        if (!SDL_GamepadConnected(s.pad))
        {
            std::fprintf(stderr, "[input] PlayStation pad disconnected, host pad %d\n", Slots + k);
            SDL_CloseGamepad(s.pad);
            s.pad = nullptr;
            continue;
        }
        uint32_t down = 0;
        for (int b = 0; b <= Touchpad; ++b)
            if (SDL_GetGamepadButton(s.pad, SDL_GamepadButton(b))) down |= 1u << b;
        int16_t axes[6];
        for (int a = 0; a < 6; ++a) axes[a] = SDL_GetGamepadAxis(s.pad, SDL_GamepadAxis(a));
        out[k].connected = true;
        out[k].ps5 = s.ps5;
        out[k].gamepad = ToXInput(down, axes);
    }
#endif
}

} // namespace ps_pads
