// PC display settings chosen in Options -> Display Settings (DISPLAY MODE, RESOLUTION): the
// state, the resolution list and their persistence in %LOCALAPPDATA%\TTT2Recomp\settings.ini.
// The menu natives (menu_ext.cpp) read and write it; the presenter applies it.
#pragma once
#include <cstdint>
#include <vector>

namespace display_settings {

enum Mode : int { Windowed = 0, Borderless = 1, Fullscreen = 2 };

struct Resolution { uint32_t width, height; };

// 16:9 render resolutions offered in the menu (fixed list; index = menu cursor).
const std::vector<Resolution>& Resolutions();

int GetMode();
int GetResolutionIndex();
Resolution GetResolution();
void SetMode(int mode);                 // clamps, persists, notifies the presenter
void SetResolutionIndex(int index);     // clamps, persists, notifies the presenter

// Bumped on every change; the presenter compares it to apply new settings once.
uint32_t Generation();

// Options -> Game Options -> INPUT DELAY: the game's own 2-frame input delay in offline modes,
// ON = the original, OFF = removed (input_delay.cpp). [Game] InputDelay=1|0, default 0 (OFF).
bool GetInputDelayOn();
void SetInputDelayOn(bool on);

// Options -> GRAPHICS SETTINGS (menu_ext.cpp's system UI page; the presenter applies them each
// frame). [Graphics] in settings.ini. The game itself always runs at 60 fps: its simulation steps
// once per vblank (kernel_video.cpp), so there is no frame-rate choice beyond V-SYNC.
struct Graphics
{
    bool vsync = true;          // OFF: tearing allowed (lowest latency)
    int renderScale = 0;        // index into RenderScales(): supersampling on top of RESOLUTION
    bool fxaa = false;          // FXAA on the final image
    int anisotropy = 0;         // index into Anisotropies(): 0 = the game's own (bilinear / trilinear)
    int sharpen = 0;            // 0 OFF, 1 LOW, 2 MEDIUM, 3 HIGH (contrast-adaptive)
    int brightness = 5;         // 0..10, 5 = unchanged
    bool fpsCounter = false;    // frames per second in the top-left corner
};
const std::vector<int>& RenderScales();      // percent: 100 125 150 200
const std::vector<int>& Anisotropies();      // 1 (off) 2 4 8 16
Graphics GetGraphics();
void SetGraphics(const Graphics& g);         // clamps, persists, bumps Generation()

// Options -> VOICE CHAT (voice_chat.h). [Voice] in settings.ini; Muted = the Steam ids of muted
// players, kept across sessions.
enum VoiceMode : int { VoiceOff = 0, VoiceOpenMic = 1, VoicePushToTalk = 2 };
struct Voice
{
    int mode = VoiceOpenMic;
    int pushToTalkKey = 'V';    // virtual-key code
    int volume = 8;             // 0..10
};
Voice GetVoice();
void SetVoice(const Voice& v);
bool IsVoiceMuted(uint64_t peer);
void SetVoiceMuted(uint64_t peer, bool muted);

} // namespace display_settings
