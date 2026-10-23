# TTT2-Recomp
Step back into the arena with a community-driven static recompilation of Tekken Tag Tournament 2. Built by a dedicated team of contributors, this project brings the legendary tag-team fighter to modern platforms with native PC performance, modernized rendering, and custom quality-of-life enhancements for competitive play.

## Steam recovery patch (by ThatCaliKid760)

This repository holds a patch for the Steam online code in the native PC recompilation by
[shihabal3amri](https://github.com/shihabal3amri/tekken-tag-tournament-2-pc). That project is the
recompilation this patch applies to; all credit for it goes to its author.

**What it fixes:** after the first online match, the guest could no longer find the host's lobby
until both players restarted, because the Steam helper process exited during gameplay
(upstream issue #30). The patch adds Steam helper recovery and lobby rediscovery in
`port/src/runtime/kernel/steam_bridge.cpp`.

**How to use it:** from a checkout of the original project, run:

```
git apply TTT2_Steam_Recovery_V3.patch
```

then build as described in that project's `BUILDING.md`. The patch was checked to apply cleanly
against upstream v0.1.6, and the author has tested it and confirmed it works.

**No game data is included here.** You must own the game. Tekken and all related characters and
content are the property of their respective owners. This is an unofficial fan project, not
affiliated with or endorsed by Bandai Namco or Microsoft.
# TTT2 Recomp

**by ThatCaliKid760**

A patched build of the native PC static recompilation of *Tekken Tag Tournament 2* (Xbox 360), with fixes and quality-of-life improvements for online play.

## Based on

This project is built on **[shihabal3amri/tekken-tag-tournament-2-pc](https://github.com/shihabal3amri/tekken-tag-tournament-2-pc)**, the original native Windows port by static recompilation. All credit for the port itself goes to that project and the tools it builds on ([XenonRecomp](https://github.com/hedge-dev/XenonRecomp), with [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) and [Xenia](https://github.com/xenia-project/xenia) as references).

## Patch notes

### 0.1.9
- **Update notifications:** when you pick your profile, the game checks for a newer version and shows your build number in the top right. If an update is out, click the notification to download it, then click again to install (right-click dismisses it).
- **Voice chat in the lobby:** press **R3** (or **F7** on keyboard) in the menus or an online lobby to change voice mode and volume and to mute players without leaving the lobby. The lobby's button bar shows the shortcut for your controller.
- **Steam profiles:** in that panel, select a player and press **X** (Square on PlayStation) to open their Steam profile.
- The mouse cursor hides when you're not using the mouse.
- **Fix:** with FXAA or sharpening on, the picture could be cut off at large render scales.

### 0.1.8
- **Graphics Settings:** V-Sync, render scale (supersampling), FXAA, anisotropic filtering, sharpening, brightness, FPS counter.
- **Voice Chat:** open mic or push-to-talk in online lobbies and matches, voice volume, mute players (F8 mutes everyone).
- **Updates:** check for and install new versions from inside the game (Options > Updates).
- **Fix:** Discord Join now works for friends-only sessions.

### 0.1.7 (+ hotfix)
- **PS4 / PS5 controllers:** native DualShock 4 and DualSense support.
- **Button prompts** switch between PlayStation, Xbox and keyboard icons across all menus and in-game UI (menu setting).
- **Discord Join:** join a friend's open lobby straight from Discord, no lobby search needed.
- **Text chat** in online custom lobbies, for players without a microphone.
- **Installer update mode:** updates an existing install and keeps your progress.
- **Fix:** custom character portraits no longer show as a black box.

Steam must be running in the background for online play.

## Install

1. Download the installer from the [Releases](https://github.com/thatcalikid760/TTT2-Recomp/releases) page.
2. Point it at your own *Tekken Tag Tournament 2 (USA, Europe)* Xbox 360 disc image and Title Update 3.
3. Launch the game from your Desktop or Start menu.

**You need your own copy of the game.** This project contains no game files; the installer builds them on your PC from your disc image.

## Reporting bugs

Found a crash, a graphics glitch or something online that doesn't work? [**Report a bug**](https://github.com/thatcalikid760/TTT2-Recomp/issues/new?template=bug_report.yml) (needs a free GitHub account).

Please include:
- your version (shown in the top right when you pick your profile),
- what happened and how to make it happen again,
- your log file: press Win+R, type `%LOCALAPPDATA%\TTT2Recomp` and attach `log.txt`. It's replaced every time the game starts, so grab it before launching again.

## Legal

Unofficial fan project, not affiliated with or endorsed by Bandai Namco or Microsoft. Tekken and all related characters and content are the property of their respective owners.
