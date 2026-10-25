# TTT2 Fighting Lounge (work in progress)

ONLINE MODE -> FIGHTING LOUNGE. Beta 0.0.1 is the first layer of a larger plan: a 3D lounge like TEKKEN 8's
Fighting Lounge, where players walk around as TTT2's own characters, emote, sit on benches to watch, and sit down
at arcade cabinets to fight.

## Concept art

![Courtyard lounge](.github/assets/lounge-concept-courtyard.png)
![Cabinet banks](.github/assets/lounge-concept-cabinets.png)

A temple courtyard at dusk with four banks of five cabinets (01-20), a featured-match screen, benches for
spectators, and the players as TTT2 characters.

## Beta 0.0.1 (now)

Code: `port/src/runtime/kernel/lounge.h`, `lounge.cpp`; the menu row and page in `menu_doc.cpp`.

- A lounge per network: the first game in hosts it, the others join; up to 32 players.
- LOUNGE CHAT: a text chat for everyone in the lounge (80 characters a message).
- 20 cabinets 01-20 as rows of a menu page: sit, ready, leave, a queue per cabinet.
- Both seated players ready: their games walk the online menus to PLAYER MATCH -> QUICK MATCH (unranked), and the search is pinned to
  the cabinet opponent (`ranked::PlanLoungeMatch`). Winner stays, the next in line takes the loser's seat.

Known limits and open items:

- The cabinet match itself (the pinned ranked search, the winner report, rotation) is not tested yet: it needs two
  PCs on Steam.
- The lounge closes when its host leaves.

## Grid and local courtyard (2026-10-25)

The D3D11 presenter now draws a five-column, four-row cabinet grid over the native lounge page. It reads the
existing host-authoritative seats, readiness, queues, player names, chat, and match state; it does not maintain
another lobby or change the network protocol. Both players still have to press Ready. The existing match
handoff, winner-stays rotation, and loser requeue are unchanged.

- Grid: arrow keys / controller D-pad select a cabinet; Enter / controller A sits, queues, or toggles readiness.
- X on the keyboard or controller X leaves the selected cabinet. C / RB opens the existing lounge chat.
- Tab / controller Y switches between the grid and courtyard. Escape / B returns to the grid, then leaves the lounge.
- Courtyard: WASD / D-pad or left stick moves, Shift runs, and left/right arrow keys turn the camera.
- E / A interacts with a nearby cabinet or bench. Q sits in an available cabinet seat or joins its queue without
  toggling readiness. G plays a short placeholder wave.
- Moving stands up from a bench or a cabinet occupied only by you. With two players seated, use X to leave before
  the match begins. Selecting a cabinet in the grid works from anywhere in the courtyard.

The courtyard follows the supplied four-bank coordinates, spawn at `(0, 0, -28)`, six bench anchors, watch-zone
steps, and featured-match board. It uses procedural geometry, a local block avatar, collision bounds, and a
perspective camera. Player silhouettes stand in for fighter portraits. The featured board identifies the playing
cabinet with the largest queue; it does not show match video. This is explicitly labeled **LOCAL BLOCKOUT**.

**Not implemented in this increment:** extracted stage art, animated TTT2 fighters, shared avatar transforms,
shared bench occupancy, native emotes/IK, mouse orbit/camera collision, live spectating, or match video. The
spectate action is labeled unavailable rather than launching a placeholder. The GDI painter uses sorted projected
faces, not the game's scene/depth pipeline; it is a blockout renderer, not the final character renderer.

The presenter caches a 1280x720 BGRA texture and uploads it only when the view changes. All guest menu calls and
lounge actions remain on the game thread; the render thread receives copies. Physical controller buttons are
sampled separately so the game's keyboard bindings (A/S/X/W) cannot trigger menu actions during WASD movement.
The native list remains the fallback without the hardware presenter or if its lounge resources fail.

Verification: `port/tools/test_lounge_view.py --work-dir <existing-temporary-directory>` checks layout and movement;
add `--host`, `--input`, or `--paint` to exercise the production state owner, UI input transitions, or painter.
`--paint --capture` additionally writes synthetic-state PNG previews (requires Pillow for the test only).
`port/tools/test_input_polling.py` covers the physical-only snapshot alongside the existing input regressions.
Two-PC Steam matches and in-game transitions still require manual testing; standalone checks do not establish
that the native match bridge works end to end.

## Plan

1. Menu lounge (0.0.1): cabinets, queues, chat, the match flow.
2. 3D room: one stage as the lounge, TTT2 character models as avatars, walking, a third-person camera, positions
   shared over Steam. The same cabinet and queue state as the menu page.
3. Emotes and sitting: an emote wheel, seats at cabinets and on benches, a sitting pose (TTT2 has none).
4. Spectating: watching a cabinet's match live.
5. Presentation: the featured-match screen, cabinet previews.
