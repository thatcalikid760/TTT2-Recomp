// Native PlayStation pads (DualShock 4, DualSense / DualSense Edge; USB and Bluetooth) through SDL3,
// a second host pad source beside XInput (kernel_xam.cpp PollPadsOnce). SDL is limited to its
// HIDAPI PS4 / PS5 drivers: Xbox pads stay on XInput so nothing is read twice, and DualShock 3 is
// not supported. SDL3.dll is delay-loaded; without it, or with
//   [Input] PlayStationNative=0   in settings.ini (players using DS4Windows or Steam Input,
//                                 whose virtual Xbox pad XInput already reads)
// every slot reads disconnected.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <Xinput.h>
#include <cstdint>

namespace ps_pads {

constexpr int Slots = 4;   // host pads 4-7 of the poll thread (0-3 are the XInput slots)

struct Pad
{
    bool connected = false;
    bool ps5 = false;                 // a DualSense (Create, not Share: button_prompts.h)
    XINPUT_GAMEPAD gamepad{};
};

// Pad poll thread only (SDL's joystick calls must stay on one thread). The first call reads the
// setting and starts SDL. A pad keeps its slot while connected and gets the same slot back when
// it reconnects, so the guest user it claimed stays its own.
void Poll(Pad out[Slots]);

// Pad poll thread only, like Poll. Rumble the pad in `slot` (0..Slots-1) with SDL's low / high
// frequency motors (XINPUT_VIBRATION's left / right) for 200 ms; the caller re-sends while it
// should keep running, and sends 0, 0 to stop. No pad in the slot, or no SDL: nothing happens.
void Rumble(int slot, uint16_t low, uint16_t high);

// SDL_GamepadButton / SDL_GamepadAxis values (SDL3 ABI; ps_pads.cpp static_asserts them), so the
// mapping below needs no SDL header.
enum SdlButton : int
{
    South = 0, East = 1, West = 2, North = 3, Back = 4, Guide = 5, Start = 6, LeftStick = 7,
    RightStick = 8, LeftShoulder = 9, RightShoulder = 10, DpadUp = 11, DpadDown = 12, DpadLeft = 13,
    DpadRight = 14, Touchpad = 20,
};
enum SdlAxis : int { LeftX = 0, LeftY = 1, RightX = 2, RightY = 3, LeftTrigger = 4, RightTrigger = 5 };

// One SDL gamepad sample as the XINPUT_GAMEPAD the game expects: Cross / Circle / Square /
// Triangle = A / B / X / Y, L1 / R1 = LB / RB, L2 / R2 = the triggers, L3 / R3 = the stick clicks,
// Options = START, Share (Create) or a touchpad click = BACK. `down` has bit n set for SDL button
// n. SDL's stick Y grows downward and its triggers run 0..32767; XInput's Y grows upward and its
// triggers run 0..255.
inline XINPUT_GAMEPAD ToXInput(uint32_t down, const int16_t axes[6])
{
    static constexpr struct { SdlButton from; uint16_t to; } kButtons[] = {
        { South, 0x1000 }, { East, 0x2000 }, { West, 0x4000 }, { North, 0x8000 },
        { LeftShoulder, 0x0100 }, { RightShoulder, 0x0200 }, { LeftStick, 0x0040 }, { RightStick, 0x0080 },
        { DpadUp, 0x0001 }, { DpadDown, 0x0002 }, { DpadLeft, 0x0004 }, { DpadRight, 0x0008 },
        { Start, 0x0010 }, { Back, 0x0020 }, { Touchpad, 0x0020 },
    };
    XINPUT_GAMEPAD g{};
    for (const auto& b : kButtons)
        if (down & (1u << b.from)) g.wButtons |= b.to;
    auto flip = [](int16_t v) { return int16_t(v == -32768 ? 32767 : -v); };
    auto trigger = [](int16_t v) { return uint8_t(v <= 0 ? 0 : (int(v) * 255 + 16383) / 32767); };
    g.sThumbLX = axes[LeftX];
    g.sThumbLY = flip(axes[LeftY]);
    g.sThumbRX = axes[RightX];
    g.sThumbRY = flip(axes[RightY]);
    g.bLeftTrigger = trigger(axes[LeftTrigger]);
    g.bRightTrigger = trigger(axes[RightTrigger]);
    return g;
}

} // namespace ps_pads
