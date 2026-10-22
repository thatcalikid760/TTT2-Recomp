// Button prompts that follow the controller in use (see button_prompts.h).
//
// The atlas is TTT2's bold menu font (DXT3 512x368, FNV-1a 0E4129769EBB5BD0 over its guest bytes).
// Its glyph table (32-byte records: u16 x, y at +0, u8 width, height at +16, u16 code at +18;
// found in guest memory 2026-10-21) puts each prompt icon at a fixed rectangle; the ones that name
// an Xbox control are below, with what replaces them. The R3 / L3 icons, the D-pad and the move
// arrows read the same on every device and stay.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include "button_prompts.h"
#include "keyboard_keys.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#pragma comment(lib, "gdiplus.lib")

namespace button_prompts {
namespace {

constexpr uint64_t kFontHash = 0x0E4129769EBB5BD0ull;
constexpr uint32_t kWidth = 512, kHeight = 368, kFmtDxt2_3 = 19;

enum class Kind { Face, FaceRinged, Shoulder, ShoulderRed, Trigger, TriggerRed, Menu, Stick, StickRed, Dots };
enum Control { A, B, X, Y, LB, RB, LT, RT, Start, Back, LS, RS, None };
struct Glyph { uint16_t code; uint16_t x, y; uint8_t w, h; Kind kind; Control control; };
const Glyph kGlyphs[] = {
    { 0xE024, 269, 170, 21, 21, Kind::Face, B },        { 0xE025, 155, 173, 20, 21, Kind::Face, A },
    { 0xE026,   0, 171, 21, 21, Kind::Face, Y },        { 0xE027, 360, 175, 20, 21, Kind::Face, X },
    { 0xE028, 230, 127, 23, 22, Kind::FaceRinged, B },  { 0xE029, 253, 127, 23, 22, Kind::FaceRinged, A },
    { 0xE02A, 276, 127, 23, 22, Kind::FaceRinged, Y },  { 0xE02B,   0, 128, 23, 22, Kind::FaceRinged, X },
    { 0x2282,  19, 236, 42, 17, Kind::Shoulder, LB },   { 0x2283, 198, 249, 42, 17, Kind::Shoulder, RB },
    { 0xFF1E, 424,  89, 44, 22, Kind::ShoulderRed, LB },{ 0xFF1C, 380,  89, 44, 22, Kind::ShoulderRed, RB },
    { 0x2286, 154, 109, 25, 22, Kind::Trigger, LT },    { 0x2287, 380, 111, 25, 22, Kind::Trigger, RT },
    { 0x2267, 253,  34, 27, 24, Kind::TriggerRed, LT }, { 0x2266, 226,  34, 27, 24, Kind::TriggerRed, RT },
    { 0x21D2,  96,  66, 40, 23, Kind::Menu, Start },    { 0xE036, 211,  82, 40, 23, Kind::Menu, Start },
    { 0x21D4, 251,  82, 37, 23, Kind::Menu, Back },     { 0xE037,   0,  83, 37, 23, Kind::Menu, Back },
    { 0x25CF, 192,   0, 29, 34, Kind::Stick, LS },      { 0x25CB, 163,   0, 29, 34, Kind::StickRed, LS },
    { 0x25C6, 105,   0, 29, 34, Kind::Stick, RS },      { 0x25C7, 134,   0, 29, 34, Kind::StickRed, RS },
    { 0xE014, 274,  58, 24, 24, Kind::Dots, None },     { 0xE015, 298,  58, 24, 24, Kind::Dots, None },
    { 0xE016,   0,  59, 24, 24, Kind::Dots, None },     { 0xE017,  24,  59, 24, 24, Kind::Dots, None },
    { 0xE018,  48,  59, 24, 24, Kind::Dots, None },     { 0xE019,  72,  59, 24, 24, Kind::Dots, None },
    { 0xE01A, 187,  60, 24, 24, Kind::Dots, None },     { 0xE01B, 137,  62, 24, 24, Kind::Dots, None },
    { 0xE01C, 161,  62, 24, 24, Kind::Dots, None },     { 0xE01D, 322,  65, 24, 24, Kind::Dots, None },
    { 0xE01E, 346,  65, 24, 24, Kind::Dots, None },     { 0xE01F, 370,  65, 24, 24, Kind::Dots, None },
    { 0xE020, 394,  65, 24, 24, Kind::Dots, None },     { 0xE021, 418,  65, 24, 24, Kind::Dots, None },
    { 0xE022, 442,  65, 24, 24, Kind::Dots, None },     { 0xE023, 466,  65, 24, 24, Kind::Dots, None },
};

// ---- style ---------------------------------------------------------------------------------
std::string IniPath()
{
    if (const char* e = std::getenv("TTT2_SETTINGS_INI"); e && *e) return e;
    char dir[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir));
    std::string path = (n && n < sizeof(dir)) ? std::string(dir) : std::string(".");
    path += "\\TTT2Recomp";
    CreateDirectoryA(path.c_str(), nullptr);
    return path + "\\settings.ini";
}

// -1 Auto, else a fixed Style. Written out when absent so the switch is easy to find.
int Setting()
{
    static const int setting = [] {
        const std::string ini = IniPath();
        char value[32] = {};
        GetPrivateProfileStringA("Input", "ButtonPrompts", "", value, sizeof(value), ini.c_str());
        if (!*value) { WritePrivateProfileStringA("Input", "ButtonPrompts", "Auto", ini.c_str()); return -1; }
        if (_stricmp(value, "Xbox") == 0) return int(Style::Xbox);
        if (_stricmp(value, "PlayStation") == 0 || _stricmp(value, "PS4") == 0) return int(Style::PlayStation4);
        if (_stricmp(value, "PS5") == 0 || _stricmp(value, "DualSense") == 0) return int(Style::PlayStation5);
        if (_stricmp(value, "Keyboard") == 0) return int(Style::Keyboard);
        return -1;
    }();
    return setting;
}

std::atomic<int> g_device{ int(Device::Xbox) };
std::atomic<uint32_t> g_generation{ 1 };

uint32_t KeysFingerprint()
{
    uint32_t f = 2166136261u;
    for (int c = 0; c < keyboard_keys::Count; ++c) f = (f ^ uint32_t(keyboard_keys::Key(c))) * 16777619u;
    return f;
}

// ---- drawing (GDI+) ------------------------------------------------------------------------
using namespace Gdiplus;

void StartGdiPlus()
{
    static std::once_flag once;
    std::call_once(once, [] {
        GdiplusStartupInput input;
        ULONG_PTR token = 0;
        if (GdiplusStartup(&token, &input, nullptr) != Ok) std::fprintf(stderr, "[prompts] GDI+ unavailable\n");
    });
}

Color Rgb(int r, int g, int b, int a = 255) { return Color(BYTE(a), BYTE(r), BYTE(g), BYTE(b)); }

// The biggest bold font with `text` fitting inside `box`.
void DrawLabel(Graphics& g, const std::wstring& text, RectF box, Color color, float maxSize)
{
    FontFamily family(L"Arial Black");
    const FontFamily* use = family.GetLastStatus() == Ok ? &family : FontFamily::GenericSansSerif();
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoClip);
    format.SetTrimming(StringTrimmingNone);          // the size is chosen to fit: never cut a character
    for (float size = maxSize; size >= 4.0f; size -= 0.5f)
    {
        Font font(use, size, FontStyleRegular, UnitPixel);
        RectF need;
        g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &format, &need);
        if (need.Width <= box.Width + 1.0f || size <= 4.0f)
        {
            SolidBrush brush(color);
            // A dark one-pixel shadow first, as the game's own labels have.
            SolidBrush shadow(Rgb(0, 0, 0, 110));
            RectF shifted(box.X + 0.6f, box.Y + 0.7f, box.Width, box.Height);
            if (color.GetR() > 128) g.DrawString(text.c_str(), -1, &font, shifted, &format, &shadow);
            g.DrawString(text.c_str(), -1, &font, box, &format, &brush);
            return;
        }
    }
}

void RoundRect(GraphicsPath& path, RectF r, float radius)
{
    const float d = std::min(radius * 2, std::min(r.Width, r.Height));
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

const Color kRed = Rgb(214, 28, 36);

// A light grey key (the game's LB / LT look), its label dark; `outline` red for the highlighted ones.
void DrawKey(Graphics& g, RectF r, const std::wstring& label, bool red, float radius)
{
    if (red)
    {
        GraphicsPath ring;
        RoundRect(ring, r, radius + 1.5f);
        SolidBrush redBrush(kRed);
        g.FillPath(&redBrush, &ring);
        r.Inflate(-2.0f, -2.0f);
    }
    GraphicsPath body;
    RoundRect(body, r, radius);
    LinearGradientBrush fill(PointF(0, r.Y), PointF(0, r.Y + r.Height), Rgb(250, 250, 252), Rgb(150, 152, 158));
    g.FillPath(&fill, &body);
    Pen edge(Rgb(24, 24, 28), 1.1f);
    g.DrawPath(&edge, &body);
    RectF text = r;
    text.Inflate(-2.0f, -1.0f);
    DrawLabel(g, label, text, Rgb(36, 36, 40), r.Height * 0.78f);
}

// A PlayStation face button: a dark button, a light rim, the symbol in its colour.
void DrawPsFace(Graphics& g, RectF r, Control c, bool ringed)
{
    if (ringed)
    {
        SolidBrush redBrush(kRed);
        g.FillEllipse(&redBrush, r);
        r.Inflate(-2.0f, -2.0f);
    }
    LinearGradientBrush fill(PointF(0, r.Y), PointF(0, r.Y + r.Height), Rgb(70, 72, 80), Rgb(14, 14, 18));
    g.FillEllipse(&fill, r);
    Pen rim(Rgb(205, 208, 215), 1.1f);
    g.DrawEllipse(&rim, r);
    const float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2, s = std::min(r.Width, r.Height) * 0.27f;
    const float stroke = std::max(1.6f, std::min(r.Width, r.Height) * 0.11f);
    switch (c)
    {
    case A:   // cross
    {
        Pen pen(Rgb(124, 178, 255), stroke);
        pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
        g.DrawLine(&pen, cx - s, cy - s, cx + s, cy + s);
        g.DrawLine(&pen, cx - s, cy + s, cx + s, cy - s);
        break;
    }
    case B:   // circle
    {
        Pen pen(Rgb(255, 92, 102), stroke);
        g.DrawEllipse(&pen, cx - s, cy - s, 2 * s, 2 * s);
        break;
    }
    case Y:   // triangle
    {
        Pen pen(Rgb(64, 226, 170), stroke);
        pen.SetLineJoin(LineJoinRound);
        const PointF tri[3] = { PointF(cx, cy - s * 1.1f), PointF(cx + s * 1.05f, cy + s * 0.75f), PointF(cx - s * 1.05f, cy + s * 0.75f) };
        g.DrawPolygon(&pen, tri, 3);
        break;
    }
    case X:   // square
    {
        Pen pen(Rgb(245, 130, 225), stroke);
        g.DrawRectangle(&pen, cx - s * 0.9f, cy - s * 0.9f, s * 1.8f, s * 1.8f);
        break;
    }
    default: break;
    }
}

// The OPTIONS / SHARE (CREATE) buttons: the name over a small dark button, as START / BACK are drawn.
void DrawPsMenu(Graphics& g, RectF r, const std::wstring& name, bool options)
{
    RectF text(r.X, r.Y, r.Width, r.Height * 0.56f);
    DrawLabel(g, name, text, Rgb(245, 245, 248), r.Height * 0.5f);
    const float bw = r.Width * 0.36f, bh = r.Height * 0.34f;
    RectF button(r.X + (r.Width - bw) / 2, r.Y + r.Height - bh - 1.0f, bw, bh);
    GraphicsPath path;
    RoundRect(path, button, bh / 2);
    SolidBrush dark(Rgb(30, 30, 36));
    g.FillPath(&dark, &path);
    Pen rim(Rgb(220, 30, 40), 1.0f);
    g.DrawPath(&rim, &path);
    Pen mark(Rgb(235, 235, 240), 1.0f);
    const float cy = button.Y + bh / 2, cx = button.X + bw / 2;
    if (options)                                        // the three lines on Options
        for (int i = -1; i <= 1; ++i) g.DrawLine(&mark, cx - bw * 0.2f, cy + i * bh * 0.22f, cx + bw * 0.2f, cy + i * bh * 0.22f);
    else                                                // a small upward arrow on Share / Create
    {
        g.DrawLine(&mark, cx, cy - bh * 0.3f, cx, cy + bh * 0.3f);
        g.DrawLine(&mark, cx - bw * 0.12f, cy - bh * 0.05f, cx, cy - bh * 0.3f);
        g.DrawLine(&mark, cx + bw * 0.12f, cy - bh * 0.05f, cx, cy - bh * 0.3f);
    }
}

// An analog stick seen from above-front, its label on the cap (L / R for PlayStation).
void DrawStick(Graphics& g, RectF r, const std::wstring& label, bool red)
{
    const float capH = r.Height * 0.62f;
    RectF base(r.X + r.Width * 0.12f, r.Y + r.Height * 0.42f, r.Width * 0.76f, r.Height * 0.55f);
    LinearGradientBrush baseFill(PointF(0, base.Y), PointF(0, base.Y + base.Height), Rgb(120, 122, 128), Rgb(38, 38, 44));
    g.FillEllipse(&baseFill, base);
    if (red) { Pen ring(kRed, 1.8f); g.DrawEllipse(&ring, base); }
    RectF cap(r.X + r.Width * 0.08f, r.Y + 1.0f, r.Width * 0.84f, capH);
    LinearGradientBrush capFill(PointF(0, cap.Y), PointF(0, cap.Y + cap.Height), Rgb(96, 98, 106), Rgb(24, 24, 30));
    g.FillEllipse(&capFill, cap);
    Pen rim(Rgb(200, 202, 210), 1.0f);
    g.DrawEllipse(&rim, cap);
    DrawLabel(g, label, cap, Rgb(245, 245, 248), cap.Height * 0.72f);
}

// A keyboard key's name, short enough for an icon: 3 characters on the small keys, 5 on START / BACK.
std::wstring ShortKey(int vk, size_t room)
{
    switch (vk)
    {
    case VK_UP: return L"↑";
    case VK_DOWN: return L"↓";
    case VK_LEFT: return L"←";
    case VK_RIGHT: return L"→";
    case VK_RETURN: return room >= 5 ? L"ENTER" : L"ENT";
    case VK_BACK: return room >= 4 ? L"BKSP" : L"BKS";
    case VK_SPACE: return room >= 5 ? L"SPACE" : L"SPC";
    case VK_TAB: return L"TAB";
    case VK_ESCAPE: return L"ESC";
    case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: return room >= 5 ? L"SHIFT" : L"SHF";
    case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return room >= 4 ? L"CTRL" : L"CTL";
    case VK_LMENU: case VK_RMENU: case VK_MENU: return L"ALT";
    default: break;
    }
    if (!vk) return L"-";
    const std::string name = keyboard_keys::KeyName(vk);
    std::wstring w(name.begin(), name.end());
    return w.substr(0, room);
}

int KeyOf(Control c)
{
    switch (c)
    {
    case A: return keyboard_keys::Key(keyboard_keys::A);
    case B: return keyboard_keys::Key(keyboard_keys::B);
    case X: return keyboard_keys::Key(keyboard_keys::X);
    case Y: return keyboard_keys::Key(keyboard_keys::Y);
    case LB: return keyboard_keys::Key(keyboard_keys::LB);
    case RB: return keyboard_keys::Key(keyboard_keys::RB);
    case LT: return keyboard_keys::Key(keyboard_keys::LT);
    case RT: return keyboard_keys::Key(keyboard_keys::RT);
    case Start: return keyboard_keys::Key(keyboard_keys::Start);
    case Back: return keyboard_keys::Key(keyboard_keys::Back);
    default: return 0;
    }
}

// The four-dot button diagrams for PlayStation: each Xbox colour to the button in its place
// (left X blue -> square pink, top Y orange -> triangle green, bottom A olive / green -> cross blue,
// right B red stays red).
void RecolorDots(uint8_t* rgba, uint32_t pitch, const Glyph& gl)
{
    for (uint32_t y = gl.y; y < uint32_t(gl.y + gl.h); ++y)
        for (uint32_t x = gl.x; x < uint32_t(gl.x + gl.w); ++x)
        {
            uint8_t* p = rgba + size_t(y) * pitch + size_t(x) * 4;
            const float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
            const float mx = std::max({ r, g, b }), mn = std::min({ r, g, b }), d = mx - mn;
            if (p[3] < 16 || mx < 0.2f || d / mx < 0.35f) continue;
            float hue = mx == r ? 60.0f * std::fmod((g - b) / d, 6.0f) : mx == g ? 60.0f * ((b - r) / d + 2) : 60.0f * ((r - g) / d + 4);
            if (hue < 0) hue += 360;
            float to;
            if (hue < 18 || hue >= 330) continue;          // red: circle, as before
            else if (hue < 50) to = 158;                     // orange / yellow (Y) -> triangle green
            else if (hue < 160) to = 218;                    // olive / green (A) -> cross blue
            else if (hue < 270) to = 312;                    // blue (X) -> square pink
            else continue;
            // Same saturation and brightness, the new hue.
            const float s = d / mx, v = mx, c = v * s, h6 = to / 60.0f, xx = c * (1 - std::fabs(std::fmod(h6, 2.0f) - 1));
            float rr = 0, gg = 0, bb = 0;
            if (h6 < 1) { rr = c; gg = xx; } else if (h6 < 2) { rr = xx; gg = c; } else if (h6 < 3) { gg = c; bb = xx; }
            else if (h6 < 4) { gg = xx; bb = c; } else if (h6 < 5) { rr = xx; bb = c; } else { rr = c; bb = xx; }
            const float m = v - c;
            p[0] = uint8_t(std::lround((rr + m) * 255)); p[1] = uint8_t(std::lround((gg + m) * 255)); p[2] = uint8_t(std::lround((bb + m) * 255));
        }
}

// Draw one replacement glyph into a fresh ARGB canvas and copy it over the atlas rectangle.
template <typename F> void PaintGlyph(uint8_t* rgba, uint32_t pitch, const Glyph& gl, F&& draw)
{
    Bitmap canvas(gl.w, gl.h, PixelFormat32bppARGB);
    {
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.Clear(Color(0, 0, 0, 0));
        draw(g, RectF(0.5f, 0.5f, gl.w - 1.0f, gl.h - 1.0f));
    }
    BitmapData data;
    Rect all(0, 0, gl.w, gl.h);
    if (canvas.LockBits(&all, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) return;
    for (uint32_t y = 0; y < gl.h; ++y)
    {
        const uint8_t* src = static_cast<const uint8_t*>(data.Scan0) + size_t(y) * data.Stride;
        uint8_t* dst = rgba + size_t(gl.y + y) * pitch + size_t(gl.x) * 4;
        for (uint32_t x = 0; x < gl.w; ++x)
        {
            dst[x * 4 + 0] = src[x * 4 + 2];   // BGRA -> RGBA
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    canvas.UnlockBits(&data);
}

const wchar_t* PsName(Control c, bool ps5)
{
    switch (c)
    {
    case LB: return L"L1"; case RB: return L"R1"; case LT: return L"L2"; case RT: return L"R2";
    case Start: return L"OPTIONS"; case Back: return ps5 ? L"CREATE" : L"SHARE";
    case LS: return L"L"; case RS: return L"R";
    default: return L"";
    }
}

} // namespace

void NoteInput(Device device)
{
    if (g_device.exchange(int(device)) != int(device) && Setting() < 0) g_generation.fetch_add(1);
}

Style Current()
{
    const int setting = Setting();
    const Style style = setting >= 0 ? Style(setting) : Style(g_device.load());
    if (style == Style::Keyboard)
    {
        // Rebinding a key in Controller Setup redraws its icons.
        static std::atomic<uint32_t> keys{ 0 };
        const uint32_t now = KeysFingerprint();
        if (keys.exchange(now) != now) g_generation.fetch_add(1);
    }
    return style;
}

uint32_t Generation() { Current(); return g_generation.load(); }

bool IsPromptFont(uint32_t format, uint32_t width, uint32_t height, bool tiled, const uint8_t* guest, size_t bytes)
{
    if (format != kFmtDxt2_3 || width != kWidth || height != kHeight || tiled || bytes < (kWidth / 4) * (kHeight / 4) * 16)
        return false;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size_t(kWidth / 4) * (kHeight / 4) * 16; ++i) { h ^= guest[i]; h *= 1099511628211ull; }
    return h == kFontHash;
}

void DecodeBc2(const uint8_t* blocks, uint32_t blocksX, uint32_t blocksY, uint8_t* rgba, uint32_t pitch)
{
    for (uint32_t by = 0; by < blocksY; ++by)
        for (uint32_t bx = 0; bx < blocksX; ++bx)
        {
            const uint8_t* b = blocks + (size_t(by) * blocksX + bx) * 16;
            uint64_t alpha;
            std::memcpy(&alpha, b, 8);
            uint16_t c0, c1;
            uint32_t idx;
            std::memcpy(&c0, b + 8, 2); std::memcpy(&c1, b + 10, 2); std::memcpy(&idx, b + 12, 4);
            auto expand = [](uint16_t c, int out[3]) {
                out[0] = ((c >> 11) & 31) * 255 / 31; out[1] = ((c >> 5) & 63) * 255 / 63; out[2] = (c & 31) * 255 / 31;
            };
            int pal[4][3];
            expand(c0, pal[0]); expand(c1, pal[1]);
            for (int k = 0; k < 3; ++k)                  // BC2 always interpolates four colours
            {
                pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
                pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
            }
            for (int i = 0; i < 16; ++i)
            {
                uint8_t* p = rgba + size_t(by * 4 + i / 4) * pitch + size_t(bx * 4 + i % 4) * 4;
                const int* c = pal[(idx >> (2 * i)) & 3];
                p[0] = uint8_t(c[0]); p[1] = uint8_t(c[1]); p[2] = uint8_t(c[2]);
                p[3] = uint8_t(((alpha >> (4 * i)) & 15) * 17);
            }
        }
}

void Paint(Style style, uint8_t* rgba, uint32_t width, uint32_t height, uint32_t pitch)
{
    if (style == Style::Xbox || width < kWidth || height < kHeight) return;
    StartGdiPlus();
    const bool ps = style == Style::PlayStation4 || style == Style::PlayStation5, ps5 = style == Style::PlayStation5;
    int painted = 0;
    for (const Glyph& gl : kGlyphs)
    {
        if (gl.kind == Kind::Dots)
        {
            if (ps) { RecolorDots(rgba, pitch, gl); ++painted; }
            continue;                                   // keyboard: the game's own button diagram
        }
        if (!ps && (gl.kind == Kind::Stick || gl.kind == Kind::StickRed)) continue;   // sticks mean movement
        PaintGlyph(rgba, pitch, gl, [&](Graphics& g, RectF r) {
            const bool red = gl.kind == Kind::ShoulderRed || gl.kind == Kind::TriggerRed || gl.kind == Kind::StickRed;
            if (ps)
            {
                switch (gl.kind)
                {
                case Kind::Face: case Kind::FaceRinged: DrawPsFace(g, r, gl.control, gl.kind == Kind::FaceRinged); break;
                case Kind::Shoulder: case Kind::ShoulderRed: DrawKey(g, r, PsName(gl.control, ps5), red, r.Height * 0.45f); break;
                case Kind::Trigger: case Kind::TriggerRed: DrawKey(g, r, PsName(gl.control, ps5), red, r.Height * 0.25f); break;
                case Kind::Menu: DrawPsMenu(g, r, PsName(gl.control, ps5), gl.control == Start); break;
                case Kind::Stick: case Kind::StickRed: DrawStick(g, r, PsName(gl.control, ps5), red); break;
                default: break;
                }
                return;
            }
            // Keyboard: a key cap with the key this control is bound to.
            const bool ringed = gl.kind == Kind::FaceRinged;
            const size_t room = gl.kind == Kind::Menu ? 5 : (gl.kind == Kind::Shoulder || gl.kind == Kind::ShoulderRed) ? 4 : 3;
            DrawKey(g, r, ShortKey(KeyOf(gl.control), room), red || ringed, r.Height * 0.22f);
        });
        ++painted;
    }
    std::fprintf(stderr, "[prompts] %s icons painted (%d)\n",
                 ps5 ? "PS5" : ps ? "PS4" : "keyboard", painted);
}

} // namespace button_prompts
