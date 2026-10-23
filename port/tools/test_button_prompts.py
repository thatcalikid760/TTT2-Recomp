"""Exercise the button-prompt painter (button_prompts.cpp) with no game data.

A synthetic 512x368 atlas stands in for the menu font: the Xbox style leaves it untouched, the
PlayStation and keyboard styles change pixels only inside the icon rectangles of the glyph table,
the BC2 decoder matches a hand-built block, textures other than the font (format, size, tiling,
contents) are never taken for it, and the style follows the last device in Auto, stays put under
a fixed setting, and is redrawn after a keyboard rebind.
"""
import os
import re
import subprocess
import tempfile
from pathlib import Path

from test_vcmpgtuh import compile_one

ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / "port/src/runtime/kernel"

CHECKER = r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "button_prompts.cpp"
#include "keyboard_keys.cpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "button_prompts.h"
#include "keyboard_keys.h"
using namespace button_prompts;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line%d: %s\n", __LINE__, #x); return 1; } } while (0)
struct R { int x, y, w, h; };
const R kRects[] = { @RECTS@ };
bool Inside(int x, int y) {
    for (const R& r : kRects) if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return true;
    return false;
}
std::vector<uint8_t> Atlas() {
    std::vector<uint8_t> a(512 * 368 * 4);
    for (size_t i = 0; i < a.size(); ++i) a[i] = uint8_t((i * 37) ^ (i >> 7));   // not a valid icon anywhere
    return a;
}
int main(int argc, char** argv) {
    const bool fixedXbox = argc > 1;
    // BC2: alpha nibbles 0..F, colour 0 white, colour 1 black, indices 0,1,2,3 repeating.
    uint8_t block[16];
    for (int i = 0; i < 8; ++i) block[i] = uint8_t((2 * i) | ((2 * i + 1) << 4));
    const uint16_t c0 = 0xFFFF, c1 = 0x0000; const uint32_t idx = 0xE4E4E4E4u;
    std::memcpy(block + 8, &c0, 2); std::memcpy(block + 10, &c1, 2); std::memcpy(block + 12, &idx, 4);
    uint8_t px[4 * 4 * 4];
    DecodeBc2(block, 1, 1, px, 16);
    for (int i = 0; i < 16; ++i) {
        const uint8_t* p = px + (i / 4) * 16 + (i % 4) * 4;
        const int expect[4] = { 255, 0, 170, 85 };
        CHECK(p[3] == i * 17 && p[0] == expect[i % 4] && p[1] == expect[i % 4] && p[2] == expect[i % 4]);
    }
    // Only the menu font is the menu font.
    std::vector<uint8_t> zeros(188416, 0);
    CHECK(!IsPromptFont(19, 512, 368, false, zeros.data(), zeros.size()));   // other contents
    CHECK(!IsPromptFont(20, 512, 368, false, zeros.data(), zeros.size()));
    CHECK(!IsPromptFont(19, 512, 256, false, zeros.data(), zeros.size()));
    CHECK(!IsPromptFont(19, 512, 368, true, zeros.data(), zeros.size()));
    CHECK(!IsPromptFont(19, 512, 368, false, zeros.data(), 1000));
    // Xbox: untouched. PlayStation / keyboard: changes only inside the icon rectangles.
    const std::vector<uint8_t> original = Atlas();
    for (int s = 0; s < 4; ++s) {
        std::vector<uint8_t> a = original;
        Paint(Style(s), a.data(), 512, 368, 512 * 4);
        size_t inside = 0, outside = 0;
        for (int y = 0; y < 368; ++y) for (int x = 0; x < 512; ++x) {
            const size_t p = (size_t(y) * 512 + x) * 4;
            if (std::memcmp(&a[p], &original[p], 4) == 0) continue;
            (Inside(x, y) ? inside : outside)++;
        }
        CHECK(outside == 0);
        CHECK(s == int(Style::Xbox) ? inside == 0 : inside > 2000);
    }
    // The style: the setting fixes it; Auto follows the last device; a rebind redraws the keys.
    if (fixedXbox) {
        NoteInput(Device::PlayStation5);
        CHECK(Current() == Style::Xbox);
        std::printf("PASS %u button-prompt checks with ButtonPrompts=Xbox\n", checks);
        return 0;
    }
    NoteInput(Device::PlayStation4);
    uint32_t g = Generation();
    CHECK(Current() == Style::PlayStation4);
    NoteInput(Device::PlayStation4); CHECK(Generation() == g);           // same device: nothing to redraw
    NoteInput(Device::PlayStation5); CHECK(Current() == Style::PlayStation5 && Generation() != g);
    NoteInput(Device::Keyboard); g = Generation(); CHECK(Current() == Style::Keyboard);
    keyboard_keys::Assign(keyboard_keys::A, 'J');
    CHECK(Generation() != g);                                             // the A key cap shows J now
    keyboard_keys::RestoreDefaults();
    NoteInput(Device::Xbox); CHECK(Current() == Style::Xbox);
    std::printf("PASS %u button-prompt checks: BC2 decode, font recognition, Xbox untouched, icons only in their rectangles, Auto / rebind redraw\n", checks);
}
'''


def main():
    src = (KERNEL / "button_prompts.cpp").read_text(encoding="utf-8-sig")
    rects = re.findall(r"\{ 0x[0-9A-F]{4},\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+), Kind::", src)
    assert len(rects) == 40, len(rects)
    checker = CHECKER.replace("@RECTS@", ", ".join("{%s,%s,%s,%s}" % r for r in rects))
    with tempfile.TemporaryDirectory(prefix="ttt2-prompts-") as temp:
        folder = Path(temp)
        exe = compile_one(folder, "prompts", checker, [KERNEL], "/link user32.lib")
        for setting, args in (("", []), ("Xbox", ["fixed"])):
            ini = folder / f"settings_{setting or 'auto'}.ini"
            if setting:
                ini.write_text(f"[Input]\nButtonPrompts={setting}\n")
            env = dict(os.environ, TTT2_SETTINGS_INI=str(ini))
            r = subprocess.run([str(exe)] + args, timeout=60, env=env, capture_output=True, text=True)
            if r.returncode:
                raise RuntimeError(r.stdout + r.stderr)
            print(r.stdout, end="")
        text = (folder / "settings_auto.ini").read_text()
        assert "ButtonPrompts=Auto" in text, text       # the default is written out


if __name__ == "__main__":
    main()
