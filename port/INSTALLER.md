# TTT2 PC port — installer (`ttt2_setup.exe`)

User request (2026-10-12): "build an installer like how we did for tom and jerry port where the
user will have to provide a copy of the xbox iso game and the updates". Modeled on the Tom & Jerry
installer (`D:\Projects\Tom and Jerry in War of the Whiskers (U)\port\src\setup`): same dialog
flow, worker thread, uninstaller guard, `/silent` test mode, payload-as-RCDATA.

**It ships no game data.** The player supplies the Xbox 360 disc image and Title Update 3; every
game file is produced on their PC at install time. The installer carries only our files: the
port exe, steam_api64.dll (Steamworks SDK redistributable), the four VC++ runtime DLLs the exe
imports, the readme, and an original icon.

## What it asks for

| Input | Accepted | Checked |
|---|---|---|
| Disc image | .iso, any XDVDFS layout: XGD3 redump (partition 0x2080000), XGD2 (0xFD90000), rebuilt (0) | SHA-1 of Default.xex = `a942090b...` (retail 0.0.0.2) |
| Title Update 3 | the file `TU_174Q22P_0000008000000.00000000000O2`, **or a folder** (picks the package with title 0x4E4D0859 whose Default.xexp targets 0.0.3.2; TU1/TU2 next to it are skipped) | xexp target version, then SHA-1 of the patched image |
| Install folder | default `C:\Program Files\Tekken Tag Tournament 2` | free space (~13 GB) |

## What it does (src/setup/setup_main.cpp RunInstall)

1. Opens the ISO (`xdvdfs.cpp`), verifies Default.xex.
2. Opens the TU (`stfs.cpp`, C++ port of `tools/stfs.py`): Default.xexp, data100.bin, data100.ofs.
3. Base xex + xexp -> XenonUtils `XexPatcher` -> decrypted image; SHA-1 must be `66adbc1a...`
   (the image the recompiled code was generated from) -> `game\tekken.img`.
4. Copies the 45 top-level disc files except AvatarAssetPack (dashboard's) into `game\`; the 26
   LZX containers (magic 0x0FF512ED/EE) are expanded on the way to the size nrfo.bin's record
   gives (`game_data.cpp ExpandContainer`, streaming per 0x20000 chunk, libmspack lzxd).
5. TU3's data100.bin expanded (7,411,280 bytes) + data100.ofs -> `game\update\` (the game's
   `update:` device).
6. Writes the payload, copies itself as `uninstall.exe`, shortcuts, HKLM uninstall key.
A failed or cancelled install deletes the files it wrote (file by file) and empty folders.

Installed mode (runtime `main.cpp InstalledLayout`): started with no arguments and
`<exe dir>\game\tekken.img` present, the exe defaults `TTT2_VFS_ROOT=<exe dir>\game`,
`TTT2_GPU_HW=1`, `TTT2_FBDUMP=0`, and with no stderr attached logs to
`%LOCALAPPDATA%\TTT2Recomp\log.txt`. All game writes already go to `%LOCALAPPDATA%\TTT2Recomp`
(saves, settings.ini, shader cache, net), so a read-only Program Files install works.

The shipped exe is `tekken_recomp.exe` with its PE subsystem switched to WINDOWS by `editbin`
(CMake target `ttt2_game_exe` -> `build-o1\payload\Tekken Tag Tournament 2.exe`): no console
window. The dev exe stays a console program. Both carry the icon + VERSIONINFO (`game.rc.in`).

## Build

    tools\build_port.cmd build-o1 1 8 ON     (configures; builds the game)
    tools\build_setup.cmd build-o1 8         (-> build-o1\setup\ttt2_setup.exe + ttt2_setup_probe.exe)

`ttt2_setup.exe` = requireAdministrator (the one to ship). `ttt2_setup_probe.exe` = the same
program as asInvoker, for scripted `/silent` installs into a test folder (no HKLM key then).
Manifests are generated (`setup.manifest.in`) and embedded through the .rc (`/MANIFEST:NO`), with
the Common Controls v6 dependency. /MT: the installer itself needs no redist.
Version: `TTT2_VERSION` in CMakeLists (VERSIONINFO, Add or remove programs).

## Test

    ttt2_setup_probe.exe /silent "<disc.iso>" "<TU file or folder>" "D:\ttt2_install_test\Tekken Tag Tournament 2"
    -> exit code 0/1, log next to the exe (ttt2_setup_probe.exe.log)
    python -I port\tools\verify_install.py "D:\ttt2_install_test\Tekken Tag Tournament 2"
    -> every game file vs re\game_files_x + re\decrypted\Default_patched_tu3.bin, byte for byte

Uninstall: `uninstall.exe` (or `/uninstall [/quiet]`). It only deletes a folder that has
`game\tekken.img` and `steam_api64.dll` (checked again right before deleting) and keeps
`%LOCALAPPDATA%\TTT2Recomp` unless the box is ticked. **Never tick that box on this PC**: it
holds the dev saves and test save roots.

## Status

- 2026-10-12: built; `/silent` install from the ISO + the `Updates` folder: exit 0 in 29 s,
  13 GB, 48/48 game files identical to the dev tree (including tekken.img). Same with the TU3
  file itself. Bug found: STFS chains starting in blocks 0..0xA9 need the hash table at backing
  block -1 (0xB000); unsigned arithmetic lost them (`stfs.cpp NextBlock`).
- Refusals measured: TU1 given -> "That is the title update for game version 0.0.1.2 ..."; a TU
  file given as the ISO -> "not an Xbox 360 disc image"; both exit 1, no folder created.
- Installed copy started like a shortcut (no args, no console, cwd = install folder; test save
  root + XUID sign-in): boots to the main menu with the test profile's records; log at
  `%LOCALAPPDATA%\TTT2Recomp\log.txt`, readable while the game runs (`_wfreopen`, not the
  non-sharing `_wfreopen_s`). Log volume: ~3 MB in the first 90 s, mostly the 49 generated-code
  `[probe]` diagnostics, each capped at 3000 lines, so bounded.
- `uninstall.exe /quiet`: install folder gone (exe removed by the delayed cmd),
  `%LOCALAPPDATA%\TTT2Recomp` untouched. Dialogs (install + uninstall) render with v6 controls.
- Rebuilding the game relinks the installer (OBJECT_DEPENDS on the payload works).
- Shareable copy: `dist\TTT2-PC-port-setup-0.1.0.exe` (unsigned: SmartScreen will ask; "More
  info" -> "Run anyway").
- Not done: a real UAC/GUI install into Program Files (registry entry, shortcuts) - the probe
  cannot write HKLM; the user's own run of ttt2_setup.exe covers it.
