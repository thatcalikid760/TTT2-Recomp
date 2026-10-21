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
against upstream v0.1.6. It has not been play-tested end to end with two players on Steam yet.

**No game data is included here.** You must own the game. Tekken and all related characters and
content are the property of their respective owners. This is an unofficial fan project, not
affiliated with or endorsed by Bandai Namco or Microsoft.
