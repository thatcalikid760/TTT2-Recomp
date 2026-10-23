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

// Options -> Display Settings image options, applied live (the presenter, the texture samplers):
//   SHARPNESS 0 (OFF) .. 10: contrast adaptive sharpening of the scaled image   [Display] Sharpness
//   ANTI-ALIASING 0 OFF, 1 FXAA (at render resolution)                          [Display] AntiAliasing
//   TEXTURE FILTERING 0 STANDARD (the game's own filters, as on the console),
//     1..4 anisotropic 2x / 4x / 8x / 16x for mipmapped textures             [Display] TextureFilter
// All default to 0: the picture as the game draws it. Cheap to read (atomics): the renderer asks
// every frame and every draw.
constexpr int kMaxSharpness = 10, kMaxAntiAliasing = 1, kMaxTextureFilter = 4;
int GetSharpness();
int GetAntiAliasing();
int GetTextureFilter();
void SetSharpness(int level);
void SetAntiAliasing(int mode);
void SetTextureFilter(int mode);

// Options -> Display Settings -> VSYNC: when a finished frame reaches the screen (the presenter). The game
// runs at its own 60 frames a second either way. ON (default): at the display's next refresh, never torn.
// OFF: at once - less input lag, the picture may tear (none on a variable refresh rate display).
// [Display] VSync=1|0.
bool GetVSync();
void SetVSync(bool on);

// Options -> Display Settings -> SHADOWS: the shadows fighters and stage objects cast (the game's shadow map,
// gpu_hw_d3d11.h). OFF skips drawing them - the picture only; the game runs the same. [Display] Shadows=1|0.
bool GetShadows();
void SetShadows(bool on);

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
    int upscaler = 1;           // 0 OFF (bilinear), 1 FSR 1 (EASU, RCAS for SHARPENING) when the image is enlarged
    int fsrMode = 0;            // index into FsrModes(): with FSR 1, render that far below RESOLUTION (never below 720p)
    int antiAliasing = 0;       // 0 OFF, 1 FXAA, 2 SMAA (1x, on the final image)
    int anisotropy = 0;         // index into Anisotropies(): 0 = the game's own (bilinear / trilinear)
    int sharpen = 0;            // 0 OFF, 1 LOW, 2 MEDIUM, 3 HIGH (contrast-adaptive)
    bool motionBlur = true;     // OFF: the game's velocity pass is skipped, so its blur moves nothing
    bool shadows = true;        // OFF: the game's shadow map is not drawn (lighter on the GPU)
    int brightness = 5;         // 0..10, 5 = unchanged
    bool fpsCounter = false;    // frames per second in the top-left corner
};
const std::vector<int>& RenderScales();      // percent: 100 125 150 200
const std::vector<int>& Anisotropies();      // 1 (off) 2 4 8 16
const std::vector<int>& FsrModes();          // percent of RESOLUTION: 100 77 67 59 50
// The GRAPHICS SETTINGS presets (LOW .. ULTRA): render scale, upscaler, FSR mode, anti-aliasing,
// filtering and sharpening; the other settings stay. PresetOf: the preset `g` matches, or -1.
constexpr int kPresets = 4;
Graphics Preset(int preset, Graphics g);
int PresetOf(const Graphics& g);
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
// Options -> Game Options -> ONLINE NETCODE (rollback sessions, rollback_session.cpp), like Tekken 8's
// rollback setting: how much input delay the host picks from the ping (the host's choice applies to
// both players) and how many frames a session may predict before it waits.
//   STANDARD (the game's own delay for the ping: 3 frames below 99 ms) / PRIORITIZE RESPONSE (one frame
//   less, more rollback) / PRIORITIZE GRAPHICS (one frame more, fewer rollbacks), up to 8 frames predicted;
//   CUSTOM: CUSTOM INPUT DELAY (1..6 frames, fixed) and CUSTOM ROLLBACK (1..8 frames).
// [Online] Netcode=0..3, CustomDelay=1..6 (default 1), CustomRollback=1..8 (default 8).
// No 0: NACE's in-band delay change takes a request for 0 and never applies it (MEASURED 2026-10-19,
// the battle stayed at 4); 1 applies.
enum Netcode : int { NetStandard = 0, NetResponse = 1, NetGraphics = 2, NetCustom = 3 };
constexpr int kMinDelay = 1, kMaxCustomDelay = 6, kMaxRollback = 8;
int GetNetcode();
int GetCustomDelay();
int GetCustomRollback();
void SetNetcode(int netcode);           // clamped, persisted
void SetCustomDelay(int frames);
void SetCustomRollback(int frames);

} // namespace display_settings
