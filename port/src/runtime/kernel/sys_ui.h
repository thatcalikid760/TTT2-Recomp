// The port's system UI (PROFILE_PLAN.md): the console-side screens - Select Profile, the new
// profile's name, message boxes - drawn with the game's own menu frames and dialog window, driven
// from the game's main thread (menu_ext.cpp, end of VdSwap). While a screen is open it owns the
// controller: the game reads neutral input.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// XamShowMessageBoxUI shown in the game's dialog window: one button as its information window
// (A / B answer), two short ones as its confirmation window (A first, B second), otherwise the
// choices listed in the message under an OK button (UP / DOWN choose, A answers, B picks the
// `cancel` button and does nothing without one). The system UI has the controller meanwhile.
// `done` runs once, on the game's main thread, with the chosen button.
struct SysUiMessageBox
{
    std::string text;                       // UTF-8, '\n' line breaks
    std::vector<std::string> buttons;
    int active = 0;                         // the focused button at first
    int cancel = -1;
    std::function<void(int choice)> done;
};
// False while another message box is showing.
bool SysUiShowMessageBox(SysUiMessageBox box);

// The game asked to sign a profile in (XamShowSigninUI with nobody signed in). Its sign-in states
// draw nothing, so the last used profile is signed in directly. Returns false when the UI cannot
// take the request (the caller then reports a cancel).
bool SysUiRequestSignIn();

// True while a system screen owns the controller (XamInputGetState reports no input).
bool SysUiOwnsInput();

// True while the title logo waits for START and nobody is signed in: START / A open Select
// Profile, so the game must not see them (it would ask for a sign-in where nothing is drawn).
bool SysUiTitleWantsProfile();

// True while the name dialog takes typed text (letter keys are not mapped to buttons).
bool SysUiTextEntryActive();

// A character typed on the PC keyboard (window thread, WM_CHAR).
void SysUiOnChar(uint32_t ch);

// True while Controller Setup -> KEYBOARD waits for a key (keyboard_keys.h): keys are not mapped
// to buttons, and the window passes each pressed key on.
bool SysUiKeyCaptureActive();
// A key pressed on the PC keyboard (window thread, WM_KEYDOWN / WM_SYSKEYDOWN; left / right
// SHIFT, CTRL and ALT told apart).
void SysUiOnKey(int vk);
// The buttons player 1's game input keeps (0xFFFF = all): Controller Setup's KEYBOARD mode leaves
// the game UP / DOWN on its row.
uint16_t SysUiGameButtonMask();

// Input layer (kernel_xam.cpp): every connected controller's buttons (keyboard included, left
// stick as D-pad), read even while the game gets neutral input; and a 150 ms START press for
// player 1 (the title continues once a profile is signed in).
uint16_t HostControllerButtons();
// The triggers are not buttons: true while any connected pad holds one.
bool HostTriggerHeld();
void HostPulseStart();
