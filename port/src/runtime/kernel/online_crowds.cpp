// Options -> GRAPHICS SETTINGS -> ONLINE CROWDS. MEASURED 2026-10-24: a stage's background characters ("Mob":
// MobManager, made by sub_825B2900) are created by sub_825B5C28, which skips them when sub_82668650 - "an online
// session is on" (sub_8266C5E8 != -1) - is true, except on stage 48; the manager's sub_825B2A20 then counts how many
// to show, 0 online (at most 3 on stage 48). The game removes them online; with the setting ON, that question answers
// "no" inside those two (the other eight callers are left alone).
#include "kernel_shared.h"
#include "display_settings.h"
#include <cstdio>

extern "C" PPC_FUNC(__imp__sub_825B5C28);
extern "C" PPC_FUNC(__imp__sub_825B2A20);
extern "C" PPC_FUNC(__imp__sub_82668650);

namespace {
thread_local bool t_mobSetup = false;              // inside the background characters' code with ONLINE CROWDS on

void AsOffline(PPCContext& ctx, uint8_t* base, PPCFunc* original)
{
    const bool outer = t_mobSetup;
    t_mobSetup = outer || display_settings::GetGraphics().onlineCrowds;
    original(ctx, base);
    t_mobSetup = outer;
}
}

// A stage sets up its background characters.
PPC_FUNC(sub_825B5C28) { AsOffline(ctx, base, __imp__sub_825B5C28); }
// The manager counts how many to show.
PPC_FUNC(sub_825B2A20) { AsOffline(ctx, base, __imp__sub_825B2A20); }

// Is an online session on? -> r3 (1 / 0).
PPC_FUNC(sub_82668650)
{
    __imp__sub_82668650(ctx, base);
    if (t_mobSetup && ctx.r3.u32)
    {
        static bool logged = false;
        if (!logged) { logged = true; std::fprintf(stderr, "[crowds] online match: the stage keeps its background characters\n"); }
        ctx.r3.u64 = 0;
    }
}
