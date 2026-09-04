# MGS4 SSAA and Aspect Ratio Enabler

Graphics settings for **METAL GEAR SOLID 4** in the Master Collection: internal resolution
scaling (supersampling), sharper shadows, anisotropic filtering, FXAA control, window
resolution and aspect ratio selection, and an undistorted HUD, menus and cutscenes on
ultrawide (21:9, 32:9) and 4:3 displays.

---

## Installation

Extract the zip into the game's **install folder** — the one that contains the `MGS4` folder
(Steam: right-click the game → Manage → Browse local files):

```
METAL GEAR SOLID 4\
├─ MGS4Enabler.exe
├─ README.md
├─ UltimateASILoader_LICENSE.md
└─ MGS4\
   ├─ winmm.dll                (Ultimate ASI Loader)
   └─ scripts\MGS4Enabler.asi
```

Then run **MGS4Enabler.exe** from that folder, pick your settings and save. That creates
`MGS4Enabler.settings` next to it, which `MGS4Enabler.asi` reads every time the game starts.
Changes take effect on the next launch.

If another mod already gave you an `MGS4\winmm.dll`, it is the same loader; keep whichever is
newer. The loader runs every `.asi` in `MGS4\scripts`.

> Do not put an `.asi` file in the game's install folder itself. It is not loaded from there,
> and some mods' config tools hang if they find one.

---

## Settings

Everything lives on the **Graphics** tab. Hover a setting in the tool for the long version.

| Setting | Default | What it does |
| --- | --- | --- |
| **Window Aspect Ratio** | Use Game Setting | Overrides the output resolution. Choosing 16:9, 21:9, 32:9 or 4:3 reveals a list of resolutions for that shape; leave it on Use Game Setting to keep whatever the game's own launcher selected. |
| **Ultrawide HUD** | Expanded HUD | How the HUD is laid out on a window that is not 16:9 — ultrawide or 4:3. **Stretched HUD** leaves the game's own behaviour. **Centered HUD** gives every widget its proper shape and centres the whole interface in a 16:9 area. **Expanded HUD** does that and moves the life bar, camo meter, weapon and item panels out to the real edges (the sides on an ultrawide, the top and bottom on 4:3), with the wheels and menus staying centred. Keys off the actual window size, so it also works on a native ultrawide or 4:3 display with the aspect left on Use Game Setting. No effect at 16:9. |
| **Solid Eye Overlay Fix** | on | With a Centered or Expanded HUD, keeps the labels that follow things in the world — pickup names, Solid Eye stat blocks and targeting icons — on the objects they mark instead of sliding toward the centre with the rest of the interface. Does nothing with a Stretched HUD. |
| **Menu Masking** | on | The title screen, main menu, pause menu, codec and loading screens are drawn for 16:9. On a wider or taller window this keeps them at that shape and blacks out the rest — side bars on an ultrawide, top and bottom bars on 4:3 — instead of letting the world show past their edges. Needs a Centered or Expanded HUD. No effect at 16:9. |
| **Cutscene Masking** | on | Shows cutscenes as the 16:9 picture centred in the window with black bars over the rest, so they are framed the way they were shot. Pair it with Cutscene FOV Compensation on an ultrawide, otherwise the bars just frame the game's own cropped picture. No effect at 16:9. |
| **Cutscene FOV Compensation** | on | On a window wider than 16:9 the game's cutscene camera keeps the 16:9 width and cuts the top and bottom off. This zooms the camera back out so the full 16:9 frame fits, which is what Cutscene Masking then shows. Only matters wider than 16:9. |
| **FOV Adjustment (%)** | 100 | How much of the world the gameplay camera shows, relative to stock. 120 shows 20% more in each direction, 80 shows less. Gameplay only — cutscenes are not affected. On a 21:9 window, 133 restores the vertical view you would have at 16:9, since the game otherwise crops it. |
| **Internal Resolution Scale (%)** | 100 | Renders internally above your display resolution and scales down on output. Works even if your monitor cannot exceed its native resolution. Up to 400%, with the internal buffer capped at 8192 wide — so the useful maximum depends on your window: about 400% at 1080p, 200% at 4K. |
| **Shadow Resolution Scale (%)** | 100 | Raises shadow map resolution beyond the game's highest Shadow Quality. 200 doubles it in each dimension; costs four times the shadow memory. |
| **Shadow Softness (Samples)** | 0 (off) | How many samples the game takes filtering a shadow edge — smoother, less noisy shadows. Separate from resolution: this is edge quality, not detail. The game's highest preset uses 7. |
| **Anisotropic Filtering** | 0 (off) | Maximum anisotropy for textures viewed at a steep angle — floors, walls, terrain seen edge-on. The game's highest texture setting asks for 8x; 16x is the hardware maximum and nearly free. |
| **FXAA** | on | Post-process anti-aliasing. The game has its own toggle in its display menu; this overrides it, and either way the change needs a restart. |
| **FXAA Quality** | Slow | Slow / Medium / Fast, where slower does more work and looks better. The game ships Medium and exposes this nowhere. Worth keeping on even with supersampling: extra resolution fixes geometry edges but not shader aliasing or specular sparkle. |

### Notes

- Internal Resolution Scale is **very** demanding — every 41% adds another whole screen's
  worth of pixels. The internal buffer is capped at 8192 wide whatever you ask for; at a 4K
  window 200% gives 7680×4320 and anything higher is clamped (the log says so).
- Shadow scaling is applied on top of the game's own Shadow Quality option, so leave that at
  its highest setting for the best result.
- A **4:3** window squashes the interface horizontally with a Stretched HUD, because the game's
  UI is authored for 16:9. Centered or Expanded HUD corrects it the same way it does on an
  ultrawide.
- The masking options and FOV Adjustment only apply when the window is not 16:9. They are on
  by default because that is the framing the game was made for; turn Cutscene Masking off if
  you would rather have cutscenes fill an ultrawide.
- The aiming reticle stays put at any internal resolution. The game truncated a coordinate on
  the way into its UI space, which sent the reticle off screen once the buffer was wider than
  4095; `MGS4Enabler.asi` widens those conversions. Verified at 8192×4608. The cause was
  identified by **drbermejor**'s [mgs4Ultra120](https://github.com/drbermejor/mgs4Ultra120);
  the details are in [docs/mgs4-rendering-research.md](docs/mgs4-rendering-research.md).

---

## Mod compatibility

This mod is designed to share the game with other mods. Where another mod it recognises
changes the same thing as one of these settings, **that mod's setting is used** and this one
stands aside: the tool greys the field out and shows the value the other mod has, and the ASI
logs the same. Nothing else is affected, and the other mod's files are only ever read, never
written.

Recognised today:

| Mod | Setting here | Stands aside when the other mod… |
| --- | --- | --- |
| MGSPatriotFix | Anisotropic Filtering | is installed at all |
| MGSPatriotFix | Shadow Resolution Scale (%) | has `Custom Shadow Resolution` set to anything but 0 |

Support for more mods will be added as they come up; the list lives in one table
(`shared/compat_table.hpp`) that both the ASI and the tool read.

If you used an earlier build of these fixes that shipped inside another mod, the first run of
the tool imports your graphics settings from that mod's settings file. Do this before running
that mod's own config tool, if it has one, since such tools tend to rewrite their file with
only the keys they know.

---

## Reporting a problem

A report is only useful with a log:

1. In the tool, **Troubleshooting** tab, tick **Debug Logging** and save.
2. Launch the game from Steam and reproduce the problem. If it crashes or hangs, stop there.
3. Quit the game (Task Manager if it hung).
4. Attach `logs\MGS4Enabler_Game.log` from the game's install folder. The log is rewritten
   on every launch, so copy it before starting the game again.

**A HUD element in the wrong place on an ultrawide or 4:3 screen:** with Debug Logging on and
a Centered or Expanded HUD, press **F11** in-game while the element is on screen. A record of
every HUD element being drawn at that moment goes into the log. Take a screenshot at the same
moment (Steam's F12) and attach both.

Issues: <https://github.com/BevelTipDrip/MGS4-SSAA-AspectRatio-Enabler/issues>

---

## Building

Visual Studio 2026 Build Tools (v145, C++20), Windows SDK 10. Clone with submodules, then
`build\build.cmd` builds Zydis and wxWidgets once and the solution after that; outputs land
in `bin\Release`. `build\package.ps1` makes the release zip (it needs the
[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) zip, which
is not in the repository).

The ultrawide and 4:3 layout code is a private submodule (`external/ultrawide`). Without it
the project still builds, with a stub in its place: every setting works except the Ultrawide
HUD family, which then does nothing. That component has its own licence (see Licence below).

Two configurations: **Release** is what ships. **Lab** (`build\build.cmd lab`, output in
`bin\Lab`) compiles in the research instrumentation and the lab settings file that the test
harness drives; a Release build contains none of it.

---

## Credits

ASI loading by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
(ThirteenAG) — see `UltimateASILoader_LICENSE.md`.

Libraries: [safetyhook](https://github.com/cursey/safetyhook) (hooking),
[Zydis](https://github.com/zyantific/zydis) (instruction decoding),
[spdlog](https://github.com/gabime/spdlog) (logging), [wxWidgets](https://www.wxwidgets.org/)
(the tool).

## Licence

This repository is under the MIT License — see `LICENSE.md`.

The ultrawide and 4:3 fix (the private `external/ultrawide` submodule, compiled into the release
binaries) is licensed separately, under a credit-required licence: you may use, modify and
redistribute it, in your own mods too, but the fix must be credited **directly and visibly to
BevelTipDrip** wherever it is used or distributed - on the mod page, in the README and in any
about screen, with a link to this repository. Passing it off as your own ends that permission.
The full text is in the submodule's `LICENSE.md`.
