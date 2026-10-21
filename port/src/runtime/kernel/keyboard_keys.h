// Keyboard key settings for player 1 (port/KEYBOARD_PLAN.md): every controller control has one
// keyboard key, chosen in Options -> Controller Setup -> KEYBOARD and kept as
//   [Keyboard]
//   Up=0x26 ...           (Windows virtual-key codes; 0 = no key)
// in %LOCALAPPDATA%\TTT2Recomp\settings.ini (TTT2_SETTINGS_INI overrides the file, for tests).
#pragma once
#include <cstdint>
#include <string>

namespace keyboard_keys {

enum Control : int { Up, Down, Left, Right, A, B, X, Y, LB, RB, LT, RT, LS, RS, Start, Back, Count };

// The control's key (virtual-key code, 0 = none).
int Key(int control);
// Gives `control` the key `vk`; a control that had `vk` takes `control`'s old key. Saved at once.
void Assign(int control, int vk);
void RestoreDefaults();
// True when some control has `vk`.
bool Bound(int vk);

// XInput button mask of a control (0 for the triggers LT / RT).
uint16_t Mask(int control);
// The control as the game names it ("UP", "X", "LT", "START", ...).
const char* ControlName(int control);
// A key as shown in the game ("Z", "ENTER", "NUM 4", "L SHIFT", ...); "-" for no key.
std::string KeyName(int vk);
// False for keys that cannot be assigned (ESC keeps a key in the guided setup).
bool Assignable(int vk);

} // namespace keyboard_keys
