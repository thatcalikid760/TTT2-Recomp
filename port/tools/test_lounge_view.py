"""Standalone lounge layout, input, status and painter checks; no game assets required."""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_vcmpgtuh import compile_one

KERNEL = Path(__file__).resolve().parents[1] / "src/runtime/kernel"
CHECKER = r'''
#include "lounge_view.h"
#include <cstdio>
#include <cmath>
using namespace lounge_view;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    int checks = 0;
    CHECK(CabinetPosition(0).x == -22 && CabinetPosition(4).x == -10);
    CHECK(CabinetPosition(5).z == -14 && CabinetPosition(9).x == -10);
    CHECK(CabinetPosition(10).x == 10 && CabinetPosition(14).x == 22);
    CHECK(CabinetPosition(19).x == 22 && CabinetPosition(19).z == -14);
    CHECK(SeatPosition(0, 0).z < CabinetPosition(0).z);
    CHECK(SeatPosition(5, 1).z > CabinetPosition(5).z);
    CHECK(BenchPosition(0).x == -18 && BenchPosition(5).x == 18);
    CHECK(Select(0, -1, 0) == 4 && Select(0, 0, -1) == 15);
    CHECK(Select(19, 1, 0) == 15 && Select(19, 0, 1) == 4);
    CHECK(Status(false, 0, false, false) == CabinetStatus::Empty);
    CHECK(Status(false, 1, true, false) == CabinetStatus::Waiting);
    CHECK(Status(false, 2, false, false) == CabinetStatus::Waiting);
    CHECK(Status(false, 2, true, false) == CabinetStatus::Waiting);
    CHECK(Status(false, 2, true, true) == CabinetStatus::Ready);
    CHECK(Status(true, 2, true, true) == CabinetStatus::Playing);
    std::array<Card, 20> cards{};
    CHECK(Featured(cards) == -1);
    cards[2].status = CabinetStatus::Playing; cards[2].queue = 1;
    cards[7].status = CabinetStatus::Playing; cards[7].queue = 3;
    CHECK(Featured(cards) == 7);
    cards[7].status = CabinetStatus::Empty;
    CHECK(Featured(cards) == 2);
    cards[2].status = CabinetStatus::Empty;
    CHECK(Featured(cards) == -1);
    Local local;
    CHECK(local.position.z == -28 && NearestCabinet(local.position) == -1);
    Move(local, 1, 1, false, 0.1f);
    CHECK(std::abs(std::hypot(local.position.x, local.position.z + 28) - 0.3f) < 0.0001f);
    local = {}; Move(local, 0, 1, true, 0.1f);
    CHECK(std::abs(local.position.z + 27.4f) < 0.0001f);
    local = {}; Move(local, 0, 1, true, 10.0f);
    CHECK(local.position.z <= -27.4f); // stalled frames cannot teleport
    local = {}; Move(local, 0, 1, true, -1.0f);
    CHECK(local.position.z == -28);
    local.position = {-22, 0, 8};
    CHECK(NearestCabinet(local.position) == 0);
    for (int i = 0; i < 100; ++i) Move(local, 0, 1, true, .1f);
    CHECK(local.position.z < 9); // cabinet collision
    local.position = {0, 0, 0};
    for (int i = 0; i < 200; ++i) Move(local, 1, 0, true, .1f);
    CHECK(local.position.x <= 27);
    CHECK(NearestBench(BenchPosition(5)) == 5);
    std::printf("PASS %d lounge layout/status/movement checks\n", checks);
}
'''
PAINTER = r'''
#include "lounge_view.cpp"
#include <cstdio>
#include <fstream>
using namespace lounge_view;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char** argv) {
    int checks = 0;
    std::vector<uint32_t> pixels;
    uint64_t serial = 0;
    CHECK(!Paint(pixels, serial));
    View view;
    view.visible = true; view.connected = true; view.members = 2;
    view.cards[0].players = {"PLAYER ONE", "PLAYER TWO"};
    view.cards[0].status = CabinetStatus::Waiting;
    Publish(view);
    CHECK(Paint(pixels, serial));
    CHECK(pixels.size() == Width * Height);
    CHECK(pixels[0] != pixels[360 * Width + 640]);
    CHECK((pixels[100] >> 24) == 255);
    const auto grid = pixels;
    if (argc > 1) std::ofstream(std::string(argv[1])+"/grid.bgra", std::ios::binary).write(reinterpret_cast<const char*>(pixels.data()), pixels.size()*4);
    const auto old = serial;
    Publish(view);
    CHECK(Paint(pixels, serial) && serial == old); // unchanged view keeps cached texture
    view.grid = false;
    Publish(view);
    CHECK(Paint(pixels, serial) && serial != old && pixels != grid);
    if (argc > 1) std::ofstream(std::string(argv[1])+"/courtyard.bgra", std::ios::binary).write(reinterpret_cast<const char*>(pixels.data()), pixels.size()*4);
    view.local.position = {0, 0, 22}; view.local.cameraYaw = 3.14f;
    Publish(view);
    CHECK(Paint(pixels, serial)); // near-plane clipping must be safe
    view.visible = false; Publish(view);
    CHECK(!Paint(pixels, serial));
    CHECK(!Available());
    SetAvailable(true); CHECK(Available());
    SetAvailable(false); CHECK(!Available());
    std::printf("PASS %d lounge painter/cache checks\n", checks);
}
'''

HOST = r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
namespace profiles { constexpr size_t kMaxName = 24; }
namespace xlive { uint64_t LocalPeerId() { return 1; } }
namespace toast { struct Note { std::string title; std::vector<std::string> lines; }; void Show(const Note&, int) {} }
void XSocketSendLounge(uint64_t, const uint8_t*, size_t) {}
@CORE@
@ACTION@
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    int checks = 0;
    g.host = true; g.phase = Phase::In;
    g.members = {{1,"ONE",0},{2,"TWO",0},{3,"THREE",0},{4,"FOUR",0}};
    std::vector<Out> out;
    auto action = [&](uint64_t peer, int cabinet, Action a) {
        Writer w(Act); w.U8(uint8_t(cabinet)); w.U8(a);
        Reader r(w.b.data()+2, w.b.size()-2); HostHandleLocked(peer, Act, r, out);
    };
    action(1,0,Sit); action(2,0,Sit);
    CHECK(g.cab[0].seat[0] == 1 && g.cab[0].seat[1] == 2);
    CHECK(g.cab[0].state == Open && !g.startPending);
    action(1,0,Ready);
    CHECK(g.cab[0].state == Open && g.cab[0].ready[0] && !g.cab[0].ready[1]);
    action(1,0,Ready); CHECK(!g.cab[0].ready[0]);
    action(2,0,Ready); CHECK(g.cab[0].state == Open);
    action(1,0,Ready); CHECK(g.cab[0].state == Playing && g.startPending);
    CabinetAction(1, false); CHECK(g.cab[1].seat[0] == 0);
    action(3,0,Sit); action(4,0,Sit); action(3,0,Sit);
    CHECK(g.cab[0].queue == std::vector<uint64_t>({3,4}));
    g.cab[0].winner = 2; FinishLocked(0, out);
    CHECK(g.cab[0].seat[1] == 2 && g.cab[0].seat[0] == 3);
    CHECK(g.cab[0].queue == std::vector<uint64_t>({4,1}));
    CHECK(!g.cab[0].ready[0] && !g.cab[0].ready[1] && g.cab[0].state == Open);
    auto snapshot = SnapshotLocked();
    g.cab[0] = {};
    Reader r(snapshot.b.data()+2,snapshot.b.size()-2); ApplySnapshotLocked(r);
    CHECK(g.cab[0].seat[0] == 3 && g.cab[0].seat[1] == 2 && g.cab[0].queue.size() == 2);
    CabinetAction(-1, false); CabinetAction(20, false);
    CHECK(g.cab[0].seat[0] == 3);
    std::printf("PASS %d live lounge readiness/queue/rotation/request checks\n", checks);
}
'''

INPUT = r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "lounge_view.h"
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <deque>
#include <mutex>
namespace profiles { constexpr size_t kMaxName = 24; }
namespace xlive { uint64_t LocalPeerId() { return 1; } }
namespace toast { struct Note { std::string title; std::vector<std::string> lines; }; void Show(const Note&, int) {} }
void XSocketSendLounge(uint64_t, const uint8_t*, size_t) {}
@CORE@
struct PPCContext {};
@UI@
std::array<bool, 256> keys{};
uint16_t physical = 0;
bool front = true;
int chats = 0, backs = 0;
namespace menu_host {
constexpr int SoundCancel = 0;
void PlaySound(PPCContext&, uint8_t*, int) {}
void ShowFrame(PPCContext&, uint8_t*, const char*) { ++backs; }
}
bool SysUiBoxBusy() { return false; }
void Enter() {}
void TypeChat() { ++chats; ui.typing = true; }
uint16_t HostControllerButtons() { return physical | (keys['W'] ? 0x0200 : 0) | (keys['S'] ? 0x2000 : 0) | (keys['A'] ? 0x1000 : 0) | (keys['X'] ? 0x8000 : 0); }
uint16_t HostPhysicalControllerButtons() { return physical; }
bool lounge_view::Available() { return true; }
void lounge_view::Publish(const lounge_view::View&) {}
HWND Foreground() { return reinterpret_cast<HWND>(1); }
DWORD Process(HWND, DWORD* p) { *p = GetCurrentProcessId() + (front ? 0 : 1); return 1; }
SHORT Key(int k) { return keys[k] ? SHORT(0x8000) : 0; }
#define GetForegroundWindow Foreground
#define GetWindowThreadProcessId Process
#define GetAsyncKeyState Key
@PRESSED@
@VIEW@
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    int checks = 0;
    PPCContext ctx;
    g.host = true; g.phase = Phase::In;
    g.members = {{1,"ONE",0},{2,"TWO",0}};
    ui.pageUp = true;
    auto tick = [&](bool idle = true) { ui.viewTickMs = NowMs()-100; ViewFrame(ctx,nullptr,Pressed(),idle); };
    keys[VK_RETURN] = true; tick(); // Entry edge must not seat a player.
    CHECK(g.cab[0].seat[0] == 0);
    keys.fill(false); tick(); keys[VK_RETURN] = true; tick();
    CHECK(g.cab[0].seat[0] == 1);
    tick(); CHECK(!g.cab[0].ready[0]);
    keys.fill(false); tick(); keys['X'] = true; tick();
    CHECK(ui.grid && g.cab[0].seat[0] == 0); // X leaves, not keyboard-mapped Y.
    keys.fill(false); tick(); keys[VK_TAB] = true; tick();
    CHECK(!ui.grid); tick(); CHECK(!ui.grid);
    keys.fill(false); tick();
    float z = ui.local.position.z;
    keys['W'] = true; tick(); CHECK(ui.local.position.z > z && chats == 0);
    keys.fill(false); keys['S'] = true; tick(); CHECK(!ui.grid && backs == 0);
    keys.fill(false); keys['A'] = true; tick(); CHECK(ui.local.position.x < 0 && g.cab[0].seat[0] == 0);
    keys.fill(false); tick(); physical = 0x8000; tick(); CHECK(ui.grid);
    physical = 0; tick(); physical = 0x1000; tick(); CHECK(g.cab[0].seat[0] == 1);
    physical = 0; tick();
    keys['C'] = true; tick(); CHECK(chats == 1);
    tick(false); keys.fill(false); keys[VK_RETURN] = true; tick(false);
    ui.typing = false; g.cab[0].seat[1] = 2;
    tick(); CHECK(!g.cab[0].ready[0]); // Closing a dialog cannot confirm readiness.
    keys.fill(false); tick(); keys[VK_RETURN] = true; tick();
    CHECK(g.cab[0].ready[0] && g.cab[0].state == Open);
    keys.fill(false); tick(); front = false; keys[VK_ESCAPE] = true; tick();
    CHECK(backs == 0);
    front = true; tick(); CHECK(backs == 0);
    keys.fill(false); tick(); keys[VK_ESCAPE] = true; tick(); CHECK(backs == 1);
    std::printf("PASS %d lounge keyboard/controller/focus/dialog checks\n", checks);
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--paint", action="store_true")
    parser.add_argument("--host", action="store_true")
    parser.add_argument("--input", action="store_true")
    parser.add_argument("--negative-control", action="store_true", help="Require the input test to reject a broken key-edge guard")
    parser.add_argument("--capture", action="store_true", help="Save painter test PNGs in --work-dir (requires Pillow)")
    args = parser.parse_args()
    if args.negative_control and not args.input:
        parser.error("--negative-control requires --input")
    checker = PAINTER if args.paint else CHECKER
    if args.host or args.input:
        source = (KERNEL / "lounge.cpp").read_text(encoding="utf-8-sig")
        # Compile the production state owner unchanged; only the external transport and toast sinks are inert.
        core = source[source.index("constexpr uint8_t kVersion"):source.index("// ---- joining and leaving")]
        action = source[source.index("void CabinetAction("):source.index("void ViewFrame(")]
        checker = HOST.replace("@CORE@", core).replace("@ACTION@", action)
        if args.input:
            ui = source[source.index("struct RowText"):source.index("std::string Two(")]
            pressed = source[source.index("uint16_t Pressed()"):source.index("constexpr int kEntries")]
            view = source[source.index("lounge_view::View LoungeView()"):source.index("void PageFrame(")]
            checker = INPUT.replace("@CORE@", core).replace("@UI@", ui).replace("@PRESSED@", pressed).replace("@VIEW@", view)
    if args.negative_control:
        guard = "const uint32_t hit = ui.overlayInput ? held & ~ui.keys : 0;"
        assert guard in checker
        checker = checker.replace(guard, "const uint32_t hit = held;")
    with tempfile.TemporaryDirectory(prefix="lounge-test-", dir=args.work_dir) as folder:
        exe = compile_one(Path(folder), "lounge", checker,
                          [KERNEL], "/link gdi32.lib user32.lib" if args.paint else "")
        result = subprocess.run([str(exe)] + ([folder] if args.capture and args.paint else []), capture_output=True, text=True)
        print(result.stdout + result.stderr, end="")
        if args.negative_control:
            if result.returncode == 0 or "FAIL" not in result.stderr:
                raise RuntimeError("Negative control did not fail its assertion")
            print("PASS negative control: broken entry/held-key guard detected")
        else:
            result.check_returncode()
        if args.capture and args.paint:
            from PIL import Image
            for name in ("grid", "courtyard"):
                raw = (Path(folder) / (name + ".bgra")).read_bytes()
                Image.frombytes("RGBA", (1280, 720), raw, "raw", "BGRA").save(args.work_dir / ("lounge-" + name + ".png"))

if __name__ == "__main__":
    main()
