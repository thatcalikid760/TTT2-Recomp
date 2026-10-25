#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "lounge_view.h"
#include <atomic>
#include <cstdio>
#include <mutex>

namespace lounge_view {
namespace {
std::mutex mutex;
View current;
uint64_t generation = 1;
std::atomic<bool> available{false};
constexpr COLORREF Background = RGB(12, 18, 27), Panel = RGB(23, 32, 44), Border = RGB(55, 72, 88);
constexpr COLORREF Text = RGB(237, 242, 246), Muted = RGB(163, 180, 196), Accent = RGB(70, 220, 230);
COLORREF Color(CabinetStatus s) {
    switch (s) {
    case CabinetStatus::Playing: return RGB(255, 115, 125);
    case CabinetStatus::Ready: return RGB(95, 225, 145);
    case CabinetStatus::Waiting: return RGB(115, 180, 255);
    default: return Muted;
    }
}
const char* Label(CabinetStatus s) {
    switch (s) {
    case CabinetStatus::Playing: return "PLAYING";
    case CabinetStatus::Ready: return "READY";
    case CabinetStatus::Waiting: return "WAITING";
    default: return "EMPTY";
    }
}
std::string Name(int i) { char b[32]; std::snprintf(b, sizeof(b), "CABINET %02d", i + 1); return b; }

class Canvas {
public:
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ oldBitmap = nullptr, oldFont = nullptr;
    HFONT fonts[3]{};
    void* bits = nullptr;
    Canvas() {
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = Width; info.bmiHeader.biHeight = -Height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap) oldBitmap = SelectObject(dc, bitmap);
        const int sizes[] = {18, 24, 36};
        for (int i = 0; i < 3; ++i)
            fonts[i] = CreateFontW(-sizes[i], 0, 0, 0, i ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        oldFont = SelectObject(dc, fonts[0]);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, GetStockObject(DC_BRUSH)); SelectObject(dc, GetStockObject(DC_PEN));
    }
    ~Canvas() {
        if (dc) { SelectObject(dc, oldBitmap); SelectObject(dc, oldFont); DeleteDC(dc); }
        if (bitmap) DeleteObject(bitmap);
        for (auto f : fonts) if (f) DeleteObject(f);
    }
    bool Good() const { return dc && bits && fonts[0] && fonts[1] && fonts[2]; }
    void Box(int x, int y, int w, int h, COLORREF fill, COLORREF edge) {
        SetDCBrushColor(dc, fill); SetDCPenColor(dc, edge); Rectangle(dc, x, y, x+w, y+h);
    }
    void Circle(int x, int y, int r, COLORREF fill) {
        SetDCBrushColor(dc, fill); SetDCPenColor(dc, fill); Ellipse(dc, x-r, y-r, x+r, y+r);
    }
    void TextAt(int x, int y, int w, const std::string& text, COLORREF color = Text, int size = 0) {
        SelectObject(dc, fonts[size]); SetTextColor(dc, color);
        RECT r{x, y, x+w, y+48};
        DrawTextA(dc, text.c_str(), int(text.size()), &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    void Portrait(int x, int y, bool occupied, COLORREF color) {
        Circle(x, y, 21, Border);
        if (occupied) { Circle(x, y-7, 7, color); Box(x-11, y+3, 22, 12, color, color); }
        else TextAt(x-5, y-12, 18, "-", Muted);
    }
    void Grid(const View& v) {
        for (int i = 0; i < Cabinets; ++i) {
            const Card& c = v.cards[i];
            int x = 24 + (i%5)*176, y = 144 + (i/5)*116;
            Box(x, y, 164, 104, Panel, v.selected == i ? Accent : Border);
            if (v.selected == i) Box(x, y, 164, 3, Accent, Accent);
            TextAt(x+10, y+8, 148, Name(i), v.selected == i ? Accent : Text);
            Portrait(x+38, y+52, !c.players[0].empty(), Color(c.status));
            Portrait(x+120, y+52, !c.players[1].empty(), Color(c.status));
            TextAt(x+72, y+39, 24, "vs", Muted);
            TextAt(x+10, y+78, 94, Label(c.status), Color(c.status));
            TextAt(x+105, y+78, 52, "Q " + std::to_string(c.queue), Muted);
        }
        const Card& c = v.cards[v.selected];
        Box(920, 144, 336, 452, Panel, Border);
        TextAt(940, 160, 296, Name(v.selected), Accent, 1);
        TextAt(940, 200, 296, Label(c.status), Color(c.status));
        for (int i = 0; i < 2; ++i) {
            Portrait(960, 248+i*46, !c.players[i].empty(), Color(c.status));
            TextAt(994, 226+i*46, 242, c.players[i].empty() ? "OPEN SEAT" : c.players[i]);
            TextAt(994, 248+i*46, 242, c.players[i].empty() ? "" : (c.ready[i] ? "READY" : "NOT READY"), c.ready[i] ? Color(CabinetStatus::Ready) : Muted);
        }
        std::string action = c.mine >= 0 ? (c.status == CabinetStatus::Playing ? "MATCH IN PROGRESS" : "ENTER / A: TOGGLE READY")
            : c.queued ? "QUEUED  #" + std::to_string(c.queued) : "ENTER / A: SIT OR JOIN QUEUE";
        TextAt(940, 336, 296, action, Accent);
        TextAt(940, 366, 296, "X: LEAVE CABINET", Muted);
        TextAt(940, 398, 296, "TAB / Y: ENTER 3D COURTYARD");
        TextAt(940, 430, 296, "SPECTATE: NOT AVAILABLE", Muted);
        TextAt(940, 470, 296, "NEXT UP", Accent);
        for (int i = 0; i < 3; ++i) TextAt(940, 500+i*26, 296, c.next[i].empty() ? "-" : std::to_string(i+1)+". "+c.next[i], Muted);
    }

    struct Face { std::vector<Vec3> vertices; COLORREF color; float depth = 0; };
    std::vector<Face> faces;
    Vec3 eye;
    float sy = 0, cy = 1;
    Vec3 Camera(Vec3 p) const {
        const float x = p.x-eye.x, y = p.y-eye.y, z = p.z-eye.z;
        const float forward = x*sy + z*cy;
        return {x*cy-z*sy, y*.96f+forward*.28f, -y*.28f+forward*.96f};
    }
    void Quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, COLORREF color) {
        Face face{{Camera(a), Camera(b), Camera(c), Camera(d)}, color};
        for (Vec3 p : face.vertices) face.depth += p.z*.25f;
        faces.push_back(std::move(face));
    }
    void Block(Vec3 p, float w, float h, float depth, COLORREF color) {
        const float x=p.x-w/2, X=p.x+w/2, y=p.y, Y=p.y+h, z=p.z-depth/2, Z=p.z+depth/2;
        COLORREF side = RGB(GetRValue(color)*3/4, GetGValue(color)*3/4, GetBValue(color)*3/4);
        Quad({x,y,z},{X,y,z},{X,Y,z},{x,Y,z},color);
        Quad({X,y,Z},{x,y,Z},{x,Y,Z},{X,Y,Z},color);
        Quad({x,y,Z},{x,y,z},{x,Y,z},{x,Y,Z},side);
        Quad({X,y,z},{X,y,Z},{X,Y,Z},{X,Y,z},side);
        Quad({x,Y,z},{X,Y,z},{X,Y,Z},{x,Y,Z},color);
    }
    POINT Project(Vec3 p) const {
        return {LONG(std::clamp(640+520*p.x/p.z, -16000.0f, 16000.0f)),
                LONG(std::clamp(350-520*p.y/p.z, -16000.0f, 16000.0f))};
    }
    void WorldText(Vec3 p, const std::string& text, COLORREF color) {
        p = Camera(p);
        if (p.z < .2f) return;
        POINT at = Project(p);
        if (at.x < -100 || at.x > Width+100 || at.y < 115 || at.y > 590) return;
        TextAt(at.x-80, at.y, 200, text, color);
    }
    void Scene(const View& v) {
        const Local& local = v.local;
        sy = std::sin(local.cameraYaw); cy = std::cos(local.cameraYaw);
        eye = {local.position.x-6*sy, 4.0f, local.position.z-6*cy};
        Box(0, 112, Width, 500, RGB(40, 43, 57), RGB(40, 43, 57));
        faces.clear();
        for (int x=-28; x<28; x+=4) for (int z=-32; z<28; z+=4) {
            COLORREF color = (x+z)%8 ? RGB(61, 64, 70) : RGB(69, 71, 76);
            Quad({float(x),0,float(z)},{float(x+4),0,float(z)},{float(x+4),0,float(z+4)},{float(x),0,float(z+4)}, color);
        }
        Block({-28,0,-2}, .5f, 2, 60, RGB(92, 66, 57));
        Block({28,0,-2}, .5f, 2, 60, RGB(92, 66, 57));
        for (float x : {-25.0f, 25.0f}) for (float z : {-23.0f, 0.0f, 20.0f}) {
            Block({x,0,z}, .25f, 4, .25f, RGB(77, 53, 43));
            Block({x,3.4f,z}, .8f, .7f, .8f, RGB(238, 164, 83));
        }
        for (int i=0; i<Cabinets; ++i) {
            Vec3 p = CabinetPosition(i); const Card& card = v.cards[i];
            Block(p, 1.65f, 2.3f, 1.05f, RGB(26, 36, 45));
            float front = (i/5)%2 == 0 ? -1.0f : 1.0f;
            Block({p.x,1.1f,p.z+front*.35f}, 1.7f, .2f, .7f, Color(card.status));
            Block({p.x,1.5f,p.z+front*.54f}, 1.25f, .6f, .04f, Color(card.status));
            for (int slot=0; slot<2; ++slot) {
                Vec3 seat = SeatPosition(i, slot);
                Block({seat.x,0,seat.z}, .13f, .5f, .13f, Border);
                Block({seat.x,.5f,seat.z}, .55f, .12f, .55f, RGB(129, 58, 61));
            }
        }
        for (float x : {-18.0f, 18.0f}) {
            Block({x,0,-2}, .6f, .5f, 5.6f, RGB(115, 81, 58));
            Block({x+(x<0 ? -.35f : .35f),.5f,-2}, .15f, .65f, 5.6f, RGB(115, 81, 58));
        }
        for (int i=0; i<3; ++i) Block({0,0,18.0f+i*1.4f}, 16, .3f*(i+1), 1.3f, RGB(86, 80, 77));
        Block({0,2.5f,24}, 12.5f, 7, .5f, RGB(28, 35, 45));
        Block({0,2.8f,23.7f}, 12, 6.4f, .05f, RGB(26, 57, 65));
        // shortcut: a local block avatar until the native skeleton/motion loader is mapped.
        Vec3 p = local.position;
        for (float leg : {-.16f, .16f}) Block({p.x+leg,p.y,p.z}, .2f, .4f, .25f, Border);
        Block({p.x,p.y+.3f,p.z}, .55f, .8f, .35f, Accent);
        Block({p.x,p.y+1.1f,p.z}, .35f, .35f, .35f, RGB(237, 202, 166));
        if (local.emote > 0) Block({p.x+.45f,p.y+1,p.z}, .16f, .65f, .16f, Accent);
        std::stable_sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.depth > b.depth; });
        int saved = SaveDC(dc);
        IntersectClipRect(dc, 0, 112, Width, 612);
        for (const Face& f : faces) {
            std::vector<Vec3> clipped;
            Vec3 prev = f.vertices.back();
            for (Vec3 next : f.vertices) {
                if ((prev.z >= .2f) != (next.z >= .2f)) {
                    float t = (.2f-prev.z)/(next.z-prev.z);
                    clipped.push_back({prev.x+t*(next.x-prev.x), prev.y+t*(next.y-prev.y), .2f});
                }
                if (next.z >= .2f) clipped.push_back(next);
                prev = next;
            }
            if (clipped.size() < 3) continue;
            POINT points[8];
            for (size_t i=0; i<clipped.size(); ++i) points[i] = Project(clipped[i]);
            SetDCBrushColor(dc, f.color); SetDCPenColor(dc, f.color);
            Polygon(dc, points, int(clipped.size()));
        }
        RestoreDC(dc, saved);
        for (int bank=0; bank<4; ++bank) {
            Vec3 p = CabinetPosition(bank*5+2); p.y = 3.3f;
            WorldText(p, std::string(1, char('A'+bank))+" BANK", Accent);
        }
        int featured = Featured(v.cards);
        WorldText({0,7,23.5f}, featured >= 0 ? "FEATURED " + Name(featured) : "NO ACTIVE MATCH", Text);
        const int nearest = NearestCabinet(local.position);
        if (nearest >= 0) {
            Box(380, 528, 520, 74, Panel, Accent);
            TextAt(396, 536, 488, Name(nearest)+"  /  "+Label(v.cards[nearest].status), Accent);
            TextAt(396, 565, 488, "E / A: Sit / Ready   Q: Queue   X: Leave");
        } else if (NearestBench(local.position) >= 0) {
            TextAt(420, 570, 500, "E / A: Sit on bench    Move: Stand", Accent);
        }
        TextAt(24, 118, 680, "LOCAL BLOCKOUT / placeholder avatar / no live video", Muted);
    }
    void Draw(const View& v) {
        Box(0, 0, Width, Height, Background, Background);
        TextAt(24, 20, 800, "FIGHTING LOUNGE", Text, 2);
        TextAt(24, 72, 850, "TEMPLE COURTYARD  /  " + v.connection, Muted);
        TextAt(1020, 28, 240, std::to_string(v.members)+" PLAYERS"+(v.host ? " / HOST" : ""), Accent);
        if (v.grid) Grid(v); else Scene(v);
        TextAt(24, 620, 1232, v.chat.empty() ? "C / RB: Lounge chat" : v.chat, Muted);
        TextAt(24, 656, 1232, v.grid ? "Arrows / D-pad: Select   Enter / A: Sit / Ready   X: Leave   C / RB: Chat   Tab / Y: Courtyard   Esc / B: Back"
            : "WASD / D-pad: Move   Shift: Run   Left / Right: Camera   E / A: Interact   G: Wave   Tab / Y: Cabinets   Esc / B: Back");
        TextAt(24, 686, 1232, "Both seated players must press Ready. Cabinet state is shared; courtyard movement is local.", Muted);
    }
};
} // namespace

void Publish(const View& view) {
    std::lock_guard lock(mutex);
    if (current == view) return;
    current = view;
    ++generation;
}
bool Available() { return available.load(); }
void SetAvailable(bool value) { available = value; }
bool Paint(std::vector<uint32_t>& pixels, uint64_t& serial) {
    View view;
    uint64_t next;
    {
        std::lock_guard lock(mutex);
        if (!current.visible) return false;
        next = generation;
        if (next == serial && pixels.size() == Width*Height) return true;
        view = current;
    }
    static Canvas canvas;
    if (!canvas.Good()) { SetAvailable(false); return false; }
    canvas.Draw(view);
    GdiFlush();
    const auto* source = static_cast<const uint32_t*>(canvas.bits);
    pixels.assign(source, source+Width*Height);
    for (auto& p : pixels) p |= 0xFF000000u;
    serial = next;
    return true;
}
} // namespace lounge_view
