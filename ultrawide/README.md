# MGS4 SSAA and Aspect Ratio Enabler: Ultrawide

The aspect-ratio work for MGS4 Master Collection on PC: an undistorted, centred UI at any
output aspect, corner widgets anchored at the margins they have on 16:9, and world-pinned
labels (pickup names, Solid Eye markers) kept on the objects they belong to.

This is a private module of [MGS4 SSAA and Aspect Ratio Enabler](https://github.com/BevelTipDrip/MGS4-SSAA-AspectRatio-Enabler).
MGS4 SSAA and Aspect Ratio Enabler builds with or without it: when this repository is checked out at
`external/ultrawide`, MSBuild compiles `src/fixes/aspect_ratio.cpp` from here; when it is
absent, a stub in the public tree logs that the module is not present and the game runs
without the ultrawide fixes. Nothing in the public tree describes how the fix works, and a
Release build of the ASI carries none of the research instrumentation or its strings: the
lab switches here are compile-time constants outside the Lab configuration (`shared/lab.hpp`).

This code is not open source: all rights reserved, see `LICENSE.md`.

## Layout

| Path | What |
| --- | --- |
| `src/fixes/aspect_ratio.cpp` | the fix: layout-converter hook (Layer 1) and the identity-keyed pool spread (Layer 2) |
| `src/fixes/aspect_ratio.hpp` | the interface MGS4 SSAA and Aspect Ratio Enabler includes - settings flags and `ApplyFixes()` |
| `docs/features/aspect-ratio.md` | the bug register (AR-001 ...) and the engine facts |
| `docs/features/ultrawide-hud-identity-spread.md` | how the fix works: the pool, the unwind-based identity, the tables |
| `docs/features/evidence/` | the captures the register cites |
| `tools/` | the lab harness: boot-to-state, F11 hot dump, live anchor commands, census and scan scripts |

The module depends on MGS4 SSAA and Aspect Ratio Enabler's core headers (`pch.hpp`, `game.hpp`, `mem.hpp`,
`log.hpp`, `config.hpp`) and on `render_pipeline.hpp` for the internal render size; it is
compiled as part of the `MGS4Enabler.vcxproj` project, not on its own.

## Building MGS4 SSAA and Aspect Ratio Enabler with this module

```
git clone --recurse-submodules https://github.com/BevelTipDrip/MGS4-SSAA-AspectRatio-Enabler
```

Without access to this repository the submodule checkout fails and the build falls back to
the stub. To add the module to an existing checkout: `git submodule update --init external/ultrawide`.

## Lab harness

The scripts in `tools/` expect to live in `C:\mgspf_tools` (paths are absolute there) and a game
install at `F:\SteamLibrary\steamapps\common\METAL GEAR SOLID 4`. The flow:

1. `boot_to_aim.ps1 -Deploy -LabConfig -StopAtMenu` builds nothing; it copies the built ASI to
   `MGS4\scripts\MGS4Enabler.asi`, drops the `MGS4Enabler.lab` marker so the ASI reads
   `MGS4Enabler.lab.settings` (never the user's own `MGS4Enabler.settings`), and boots to the
   main menu. The ASI removes the marker once it has read it.
2. `hotdump.ps1`, started detached, watches for F11 and writes a screenshot plus the anchor
   log for that frame to `lab3\hotdump\NN\`. The user triggers it so the element is in frame.
3. `live_anchor.txt` takes the live commands (`spread`, `world 0|1|2`, `chanbias`, `sites`, ...)
   read by the ASI each frame.
4. `strcensus.py` turns a `sites` dump into a per-channel strcode census; `scan_narrow.py` /
   `scan_wide.py` scan a decrypted `.text` dump for the 16-bit conversion sites (the dump
   itself is not in this repository - it is game code).

The ASI writes its log to `logs\MGS4Enabler_Game.log` under the game root; the harness
tails that file.

`MGS4Enabler.lab.settings.example` is the lab configuration the measurements were taken with.

## Credit

Research and measurement by BevelTipDrip, 2026. Community use is welcome; passing the work off
as your own is not.
