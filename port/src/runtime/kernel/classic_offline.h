// CLASSIC OFFLINE (port only): OFFLINE MODE with an arcade-style roster to unlock. On the main menu, with the cursor
// on OFFLINE MODE, X (key hint "CLASSIC OFFLINE") enters OFFLINE MODE with 22 fighters open; each ARCADE clear (the
// game reaching its ending) opens the next of the other 37, in a fixed order. Progress is the profile's own
// (<save root>/<XUID>/classic_offline.ini); the normal OFFLINE MODE and ONLINE MODE keep the whole roster. Once all
// 37 are open the entry is no longer offered.
#pragma once
#include "kernel_shared.h"

namespace classic_offline {

// The game's main thread, every frame (menu_ext.cpp): the main menu's key hint and X, leaving the mode.
void Frame(PPCContext& ctx, uint8_t* base);
uint16_t GameButtonMask();             // X is the hint's while it is shown

} // namespace classic_offline
