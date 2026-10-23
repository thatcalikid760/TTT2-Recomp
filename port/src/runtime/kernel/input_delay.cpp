// TTT2's built-in input delay (GitHub issues #5 / #12). Every frame, sub_8225BFC0(obj, player) hands
// gameplay one player's input from a FIFO: obj+0 is the delay in frames, obj+4+player*32 that
// player's std::deque of input frames (new ones pushed in, the oldest taken once the queue holds
// delay+1). The game sets the delay to 2 in every offline mode; Practice's "Input Delay" setting
// (online simulation) sets 5/8/11/14 (bars 4..1), and online play its own. The read of obj+0 is the
// PS3 build's `lwz r9,0(r3)` that the RPCS3 "Reduce input lag" patch turns into `li r9,0`.
// MEASURED 2026-10-18 in Practice (press DOWN -> first presented frame of the crouch, 3 tries
// each): delay 2 -> 96.6-110 ms (~6.1 frames), delay 0 -> 73-77 ms (~4.5 frames).
//
// Options -> Game Options -> INPUT DELAY (display_settings: ON = the original, OFF = removed, the
// default): OFF makes the 2 offline frames 0 - only while the game holds exactly 2 (Practice's
// simulation keeps its value) and no online session exists (both sides of an online match must
// delay alike). TTT2_INPUT_DELAY=<frames> forces a value for tests; TTT2_INPUT_TIMING=1 logs when
// player 1's input reaches gameplay.
#include "kernel_shared.h"
#include "display_settings.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

bool XSessionAnyActive();   // xsession.cpp

extern "C" PPC_FUNC(__imp__sub_8225BFC0);

namespace {
int Forced()
{
    static const int v = [] { const char* e = std::getenv("TTT2_INPUT_DELAY"); return e ? std::atoi(e) : -1; }();
    return v;
}
bool Timing()
{
    static const bool on = [] { const char* e = std::getenv("TTT2_INPUT_TIMING"); return e && *e == '1'; }();
    return on;
}
} // namespace

PPC_FUNC(sub_8225BFC0)
{
    const uint32_t obj = ctx.r3.u32, player = ctx.r4.u32;
    const uint32_t delay = GRead32(base, obj);
    const int want = Forced() >= 0 ? Forced() : (display_settings::GetInputDelayOn() ? 2 : 0);
    const bool replace = delay == 2 && uint32_t(want) != delay && !XSessionAnyActive();
    if (replace) GWrite32(base, obj, uint32_t(want));
    __imp__sub_8225BFC0(ctx, base);
    if (replace) GWrite32(base, obj, delay);             // the game's own value stays in place
    if (Timing() && player == 0)
    {
        static uint32_t last = 0;
        if (ctx.r3.u32 != last)
            std::fprintf(stderr, "[input] game used %08X at %.1f ms (delay %u%s)\n", ctx.r3.u32,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                         delay, replace ? " -> removed" : "");
        last = ctx.r3.u32;
    }
}
