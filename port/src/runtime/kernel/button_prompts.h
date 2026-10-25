// Button prompts that follow the controller in use (button_prompts.cpp). TTT2 draws its prompt
// icons (A / B / X / Y, LB / RB, LT / RT, START / BACK, LS / RS, the four-dot button diagrams) as
// characters of its bold menu font, a 512x368 DXT3 atlas. When the GPU backend uploads that atlas,
// the icons for the current style are painted over it - drawn here at run time (GDI+), so no
// artwork is shipped - and the letters stay the game's own. The style follows the device player 1
// last pressed a button on, or a setting:
//   [Input] ButtonPrompts=Auto | Xbox | PlayStation | Keyboard     in settings.ini
#pragma once
#include <cstdint>
#include <vector>

namespace button_prompts {

enum class Style : int { Xbox = 0, PlayStation4 = 1, PlayStation5 = 2, Keyboard = 3 };

// Input layer (kernel_xam.cpp): player 1 pressed something on this device.
enum class Device : int { Xbox, PlayStation4, PlayStation5, Keyboard };
void NoteInput(Device device);

// The style to draw now (the setting, or the last device in Auto). Bumps Generation() on change.
Style Current();
uint32_t Generation();                 // changes whenever the icons to draw change (style, keys)

// The menu font atlas? `guest` is its texture data as the guest stores it (DXT3, 512x368, linear).
bool IsPromptFont(uint32_t format, uint32_t width, uint32_t height, bool tiled, const uint8_t* guest, size_t bytes);

// BC2 (DXT3) blocks, host layout, to RGBA8 rows of `pitch` bytes.
void DecodeBc2(const uint8_t* blocks, uint32_t blocksX, uint32_t blocksY, uint8_t* rgba, uint32_t pitch);

// Paint the icons of `style` over the decoded atlas (RGBA8, 512 wide, rows of `pitch` bytes).
// Xbox leaves it as it is.
void Paint(Style style, uint8_t* rgba, uint32_t width, uint32_t height, uint32_t pitch);

// Controller Setup's controller atlas (DXT5 512x512, tiled), checked over its guest bytes.
bool IsControllerAtlas(uint32_t format, uint32_t width, uint32_t height, bool tiled, const uint8_t* guest, size_t bytes);

// Redraw the Xbox 360 pad in that atlas (RGBA8, 512x512) as `style`'s controller or a keyboard and
// mouse with the bound keys lit. Xbox, and any other size, leaves it as it is.
void PaintController(Style style, uint8_t* rgba, uint32_t width, uint32_t height, uint32_t pitch);

} // namespace button_prompts
