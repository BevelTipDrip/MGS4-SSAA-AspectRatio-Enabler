# MGS4 SSAA and Aspect Ratio Enabler: Ultrawide

The aspect-ratio work for MGS4 Master Collection on PC: an undistorted, centred UI at any
output aspect, corner widgets anchored at the margins they have on 16:9, and world-pinned
labels (pickup names, Solid Eye markers) kept on the objects they belong to.

This folder is part of [MGS4 SSAA and Aspect Ratio Enabler](https://github.com/BevelTipDrip/MGS4-SSAA-AspectRatio-Enabler).
It was kept in a private repository from 2026-09-03 and joined this one, with its history, on
2026-11-08. `MGS4Enabler.vcxproj` compiles `src/fixes/aspect_ratio.cpp` from here, and
`pw/` holds the Peace Walker part (see `pw/README.md`). A Release build of the ASI carries none
of the research instrumentation or its strings: the lab switches here are compile-time
constants outside the Lab configuration (`shared/lab.hpp`).

This code is licensed separately from the rest of the repository: free to use and build on,
but the ultrawide and 4:3 fix must be credited directly and visibly to BevelTipDrip. See
`LICENSE.md`.

## Layout

| Path | What |
| --- | --- |
| `src/fixes/aspect_ratio.cpp` | the fix: layout-converter hook (Layer 1) and the identity-keyed pool spread (Layer 2) |
| `src/fixes/aspect_ratio.hpp` | the interface the plugin includes - settings flags and `ApplyFixes()` |
| `docs/features/aspect-ratio.md` | the bug register (AR-001 ...) and the engine facts |
| `docs/features/ultrawide-hud-identity-spread.md` | how the fix works: the pool, the unwind-based identity, the tables |
| `docs/features/letterbox.md` | menu and cutscene masking, the cutscene FOV, and the D3D11/D3D12 picture fit |
| `pw/` | the Peace Walker wide canvas and its notes |

The code depends on the plugin's core headers (`pch.hpp`, `game.hpp`, `mem.hpp`, `log.hpp`,
`config.hpp`) and on `render_pipeline.hpp` for the internal render size; it is compiled as
part of `MGS4Enabler.vcxproj`, not on its own.

The lab harness that took the measurements (boot-to-state, the F11 hot dump, live anchor
commands, census and scan scripts) and the screen captures the register cites are not in this
repository; the documents name each capture where they use it.

## Credit

Ultrawide and 4:3 HUD, menu and cutscene fix by BevelTipDrip. Research and measurement by
BevelTipDrip, 2026. Community use is welcome; passing the work off as your own is not.
