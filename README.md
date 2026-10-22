# TTT2 Recomp

**by ThatCaliKid760**

A patched build of the native PC static recompilation of *Tekken Tag Tournament 2* (Xbox 360), with fixes and quality-of-life improvements for online play.

## Based on

This project is built on **[shihabal3amri/tekken-tag-tournament-2-pc](https://github.com/shihabal3amri/tekken-tag-tournament-2-pc)**, the original native Windows port by static recompilation. All credit for the port itself goes to that project and the tools it builds on ([XenonRecomp](https://github.com/hedge-dev/XenonRecomp), with [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) and [Xenia](https://github.com/xenia-project/xenia) as references).

## What's changed in TTT2 Recomp

- **Online lobby fix** ([upstream issue #30](https://github.com/shihabal3amri/tekken-tag-tournament-2-pc/issues/30)): after the first online match, the guest could no longer find the host's lobby until both players restarted. The Steam helper now recovers if it exits mid-game, and lobbies are republished so they can be found again.
- Launcher and installer branded "TTT2 Recomp by ThatCaliKid760".

### Planned

- Native PS4/PS5 controller support with PlayStation button prompts
- Discord invite/join

## Install

1. Download the installer from the [Releases](https://github.com/thatcalikid760/TTT2-Recomp/releases) page.
2. Point it at your own *Tekken Tag Tournament 2 (USA, Europe)* Xbox 360 disc image and Title Update 3.
3. Launch the game from your Desktop or Start menu.

**You need your own copy of the game.** This project contains no game files; the installer builds them on your PC from your disc image.

## Legal

Unofficial fan project, not affiliated with or endorsed by Bandai Namco or Microsoft. Tekken and all related characters and content are the property of their respective owners.
