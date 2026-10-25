"""Compile production menu input handlers with inert guest/UI boundaries.

Covers issue #8 (voice shortcuts stealing Controller Setup input) and the
existing issue #6 fix (key capture waiting on unrelated held keys).
No game data, real window, audio, or settings writes are required.
"""
import argparse
from pathlib import Path
import subprocess

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / "port/src/runtime/kernel"

CHECKER = r'''
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>
#include "keyboard_keys.h"
struct PPCContext {};
constexpr int VK_F7 = 0x76, VK_ESCAPE = 0x1B;
bool keys[256]{};
short GetAsyncKeyState(int key) { return keys[key] ? short(0x8000) : 0; }
uint16_t buttons = 0;
uint16_t HostControllerButtons() { return buttons; }
bool HostTriggerHeld() { return false; }
bool keyui_Foreground() { return true; }
std::string frame = "FrameControlerSetup";
namespace menu_host {
std::string CurrentFrame(PPCContext&, uint8_t*) { return frame; }
}
uint32_t mode = 16;
uint32_t GRead32(uint8_t*, uint32_t address) { return address == 0x82F30B94 ? 0x1000 : mode; }
constexpr uint16_t kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kA = 0x1000, kB = 0x2000;
constexpr int kSoundDecide = 1, kSoundCancel = 2, kSoundCursor = 3;
void MenuSound(PPCContext&, uint8_t*, int) {}
void MenuSetText(PPCContext&, uint8_t*, const std::string&, const std::string&) {}
namespace display_settings {
struct Voice { int mode = 0, volume = 5; };
Voice GetVoice() { return {}; }
void SetVoice(Voice) {}
bool IsVoiceMuted(uint64_t) { return false; }
void SetVoiceMuted(uint64_t, bool) {}
}
namespace voice_chat { void OpenSteamProfile(uint64_t) {} }
std::atomic<bool> g_owns{false}, g_boxOwns{false}, g_kbOwns{false};
@VOICE_STATE@
int opens = 0, closes = 0;
void ShowVoicePanel(PPCContext&, uint8_t*, VoicePanel&) { ++opens; }
void HideDialog(PPCContext&, uint8_t*) { ++closes; }
void UpdateVoicePanel(PPCContext&, uint8_t*, VoicePanel&) {}
@VOICE_FRAME@
namespace keyui {
@KEY_STATE@
std::string CurrentFrame(PPCContext&, uint8_t*) { return frame; }
bool Foreground() { return true; }
int MenuCursor(PPCContext&, uint8_t*, const std::string&) { return 1; }
void Edit(PPCContext&, uint8_t*, State&, uint16_t, bool) {}
void Draw(PPCContext&, uint8_t*, State&, bool) {}
@KEY_FRAME@
}
unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    PPCContext ctx;
    auto tick = [&] { VoicePanelFrame(ctx, nullptr); };
    auto release = [&] { buttons = 0; keys[VK_F7] = false; tick(); };
    for (uint32_t allowed : {29u, 35u}) {
        mode = allowed;
        frame = "FrameControlerSetup";
        release(); buttons = kRightStick; tick();
        CHECK(!g_vp.showing && !g_vpOwns && opens == closes);
        // Leaving setup with RS held must not replay the suppressed press.
        frame = "FrameMainMenu"; tick();
        CHECK(!g_vp.showing);
        release(); buttons = kRightStick; tick();
        CHECK(g_vp.showing && g_vpOwns && opens == closes + 1);
        tick(); CHECK(g_vp.showing); // holding the opening key does not close
        release(); buttons = kRightStick; tick();
        CHECK(!g_vp.showing && g_vp.releasing && g_vpOwns);
        release(); CHECK(!g_vpOwns && !g_vp.releasing && opens == closes);
        frame = "FrameControlerSetup"; keys[VK_F7] = true; tick();
        CHECK(!g_vp.showing && !g_vpOwns);
        frame = "FrameMainMenu"; tick(); CHECK(!g_vp.showing);
        release(); keys[VK_F7] = true; tick(); CHECK(g_vp.showing);
        release(); keys[VK_F7] = true; tick(); release();
        CHECK(!g_vpOwns && opens == closes);
    }
    // Offline menus (16: main menu, Options, Customize ...), character select, a fight.
    for (uint32_t blocked : {0u, 8u, 9u, 16u}) {
        mode = blocked;
        frame = "FrameMainMenu";
        buttons = kRightStick; keys[VK_F7] = true; tick();
        CHECK(!g_vp.showing && !g_vpOwns); release();
    }
    mode = 29;
    for (auto* owner : {&g_owns, &g_boxOwns, &g_kbOwns}) {
        *owner = true; buttons = kRightStick; keys[VK_F7] = true; tick();
        CHECK(!g_vp.showing && !g_vpOwns); release(); *owner = false;
    }
    // Issue #6: only the key ending capture and held controller buttons delay release.
    frame = "FrameControlerSetup";
    for (int captured : {int('J'), VK_ESCAPE}) {
        auto& st = keyui::g_state;
        st = {}; st.here = st.keyboard = st.editing = st.releasing = true;
        st.releaseKey = captured; keyui::g_capture = true;
        keys[captured] = true; keys[0xE5] = true; keys['U'] = true;
        keyui::Frame(ctx, nullptr);
        CHECK(st.releasing && keyui::g_capture);
        keys[captured] = false; buttons = kB;
        keyui::Frame(ctx, nullptr);
        CHECK(st.releasing && keyui::g_capture);
        buttons = 0; keyui::Frame(ctx, nullptr);
        CHECK(!st.releasing && !keyui::g_capture && st.releaseKey == 0);
        CHECK(st.editing && st.keyboard);
    }
    keyui::g_capture = true; keyui::g_state.releasing = true;
    frame = "FrameMainMenu"; keyui::Frame(ctx, nullptr);
    CHECK(!keyui::g_capture && keyui::g_gameMask == 0xFFFF && !keyui::g_state.here);
    std::printf("PASS %u menu input checks: voice context/edges, key capture release\n", checks);
}
'''


def checker(source):
    voice_state = source.split("constexpr uint32_t kVoicePanelTextId", 1)[1].split("std::string VoicePanelText", 1)[0]
    voice = source.split("void VoicePanelFrame(", 1)[1].split("// Confirm:", 1)[0]
    key = source.split("namespace keyui {", 1)[1]
    key_state = key.split("int SlotOf(", 1)[0]
    key_frame = key.split("void Frame(PPCContext& ctx, uint8_t* base)", 1)[1].split("} // namespace keyui", 1)[0]
    return (CHECKER.replace("@VOICE_STATE@", "constexpr uint32_t kVoicePanelTextId" + voice_state)
            .replace("@VOICE_FRAME@", "void VoicePanelFrame(" + voice)
            .replace("@KEY_STATE@", key_state)
            .replace("@KEY_FRAME@", "void Frame(PPCContext& ctx, uint8_t* base)" + key_frame))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    source = (KERNEL / "menu_ext.cpp").read_text(encoding="utf-8-sig")
    if args.negative_control:
        guard = 'if (menu_host::CurrentFrame(ctx, base) == "FrameControlerSetup") return;'
        assert source.count(guard) == 1
        source = source.replace(guard, "")
    exe = compile_one(args.work_dir, "menu_input", checker(source), [KERNEL])
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    print(result.stdout + result.stderr, end="")
    if args.negative_control:
        assert result.returncode != 0 and "opens == closes" in result.stderr, result
        print("PASS negative control: removing the Controller Setup guard reproduces issue #8")
    elif result.returncode:
        raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
