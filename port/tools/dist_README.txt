TEKKEN TAG TOURNAMENT 2 - native PC port
=========================================

This is a native Windows port of the Xbox 360 game, made by static recompilation. It ships no
game data: the installer built the game files on this PC from YOUR OWN copy of the game (the
Xbox 360 disc image plus Title Update 3). Do not share the installed "game" folder.

Windows 10 or 11, 64-bit. A DirectX 11 graphics card. Every runtime DLL that is not part of
Windows is already in this folder.


PLAYING
-------
Start "Tekken Tag Tournament 2" from the desktop or the Start menu.

An Xbox controller (or any XInput pad) is the intended way to play; whoever presses a button
first is player 1. DualShock 4 and DualSense (PS5) controllers work directly too, over USB or
Bluetooth: Cross / Circle / Square / Triangle are A / B / X / Y, L1 / R1 the bumpers, L2 / R2 the
triggers, Options is START and Share / Create or a touchpad click is BACK. If you use DS4Windows
or Steam Input (which already turn the pad into an Xbox controller), set PlayStationNative=0 in
the [Input] section of settings.ini so each press is not read twice.
The keyboard also works as player 1:

    Arrow keys      D-pad                 A / S / Z / X   A / B / X / Y buttons
    Enter or Space  START                 Q / W           left / right bumper
    Backspace       BACK

    Alt+Enter       windowed <-> borderless full screen

Window mode (windowed / borderless / full screen) and the render resolution are in the game's
own menus: OPTIONS > DISPLAY SETTINGS. EXIT on the main menu, or closing the window, quits.


SAVES, SETTINGS AND THE LOG
---------------------------
Everything the game writes goes to

    %LOCALAPPDATA%\TTT2Recomp\

(saves and profiles, settings.ini, the shader cache, and log.txt). Uninstalling keeps that
folder unless you tick "Also delete my saved games". If something goes wrong, log.txt from
there is the file to send.

The first start of a fight compiles graphics shaders, so the first few seconds can stutter;
later runs use the cache.


ONLINE (Steam)
--------------
Online Player Match runs over Steam: no ports to open, no Xbox LIVE.

  - The Steam client must be running and signed in on both PCs.
  - Both players need this port installed from their own copy of the game. Install from the
    same installer version on both PCs.
  - Host: ONLINE MODE > PLAYER MATCH > create a session. Steam shows you as playing
    "Spacewar" - that is Valve's public test app id this port uses for now; it is expected.
  - Join: ONLINE MODE > PLAYER MATCH > search. Hosts on Steam playing this port are listed.
  - Discord: while you host a Player Match, Discord shows a joinable party (no Discord login
    needed, only the Discord app running). Friends can press Join on your profile, or you can
    invite them from a Discord chat (the + button). If the game is closed, Discord starts it and
    the join waits until a profile is signed in. Steam's "Join Game" keeps working as well.
    To turn it off: Enabled=0 in the [Discord] section of settings.ini.

The first time ONLINE MODE is opened the game shows Namco's Online Mode terms; accept them to
continue. Online leaderboards, World Arena and voice chat are not available.


UNINSTALLING
------------
Settings > Apps > Installed apps > "Tekken Tag Tournament 2 (PC port)", or run uninstall.exe in
the install folder.


LEGAL
-----
Tekken Tag Tournament 2 is (c) Bandai Namco Entertainment. This port is an unofficial fan
project, not affiliated with or endorsed by Bandai Namco or Microsoft. You must own the game.
Steamworks: steam_api64.dll is (c) Valve Corporation, redistributed under the Steamworks SDK
licence.
SDL3.dll (native DualShock 4 / DualSense input) is Simple DirectMedia Layer, (c) Sam Lantinga,
zlib licence (https://libsdl.org).
discord_partner_sdk.dll (Discord Join) is (c) Discord Inc., distributed with this program under
the Discord Social SDK Terms.
