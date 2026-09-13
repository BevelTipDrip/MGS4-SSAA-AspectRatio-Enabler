# Peace Walker

The Peace Walker build of the Enabler, `MGSPWEnabler.asi`, for METAL GEAR SOLID: Peace Walker -
Master Collection Version (Steam app 2492660). It ships in the same zip as the MGS4 build, with
its own files:

| | MGS4 | Peace Walker |
| --- | --- | --- |
| loader | `MGS4\winmm.dll` | `mgspw\winmm.dll` |
| plugin | `MGS4\scripts\MGS4Enabler.asi` | `mgspw\scripts\MGSPWEnabler.asi` |
| settings | `MGS4Enabler.settings` | `MGSPWEnabler.settings` |
| log | `logs\MGS4Enabler_Game.log` | `logs\MGSPWEnabler_Game.log` |
| manifests for other mods | `<Mod>.MGS4Enabler.ini` | `<Mod>.MGSPWEnabler.ini` |
| settings tool | `MGS4Enabler.exe`, run from the install folder | the same exe, run from the Peace Walker install folder |

The tool is one program that serves whichever game's folder it is dropped into: it looks for
`MGS4\mgs4.exe` or `mgspw\METAL GEAR SOLID PEACE WALKER.exe` beside itself (`--game MGS4` or
`--game MGSPW` chooses when both are there). Each ASI works on its own without the tool.

## What it does

The Graphics tab of the tool's Peace Walker window, all read by the Release ASI:

- **Aspect Ratio** (Use Display, 16:9, 16:10, 21:9, 32:9, 4:3): 16:9 is the game's own layout.
  Any other shape widens or heightens the PSP canvas to it: the world fills the display at
  correct proportions, the HUD and menus keep the 16:9 scale and stay centred, the full-screen
  effects follow, and the in-mission HUD groups move to the display edges with the 16:9 margin.
  Use Display takes the primary display's shape at start-up. (The aspect work itself is the
  private module; without it the setting only sizes the picture.)
- **Screen Resolution**, one list per shape: the picture on screen (the window's client, or the
  display mode in Fullscreen).
- **Render Resolution**, one list per shape: the scene at an integer multiple of the canvas
  (the game's launcher offers 3x and 4x; 8x is 4K-class, 16x is 8K-class), downscaled by the
  game's own final blit into the picture. No oversized window.
- **Window Mode** (Use Game Setting, Fullscreen, Borderless, Windowed): written into the game's
  saved display mode every launch; the in-game Options show it and can change it for the run.
  Fullscreen runs at the display's current refresh rate (the game asks for 60 Hz whatever the
  display runs, which drops a frame a second on a 120 Hz desktop). Windowed sizes the window to
  the selected screen resolution.
- **MSAA** (4x, the game's own, or Off): about 2 ms a frame at 8K.
- **Anisotropic Filtering**: 16x on every texture the game samples linearly (its own samplers
  use none; little visible change at high render scales, since the PSP-sized textures are
  magnified almost everywhere).
- **GPU Local Textures**: the game's full-size frame textures carry CPU write access and end up
  in system memory, so its three per-frame copies into them cross PCIe (5 ms each at 8K). On,
  they stay in video memory: 60 fps at 8K instead of 27.

The Lab build adds the draw census, GPU timing, a hitch log and the size experiments
([lab.md](lab.md)); the engine facts are in [engine-research.md](engine-research.md), the plan
in [NEXT-SESSION.md](NEXT-SESSION.md).

## For the code

Two rules that are not obvious from the tree:

- **The shared core is compiled from `asi\src\core`** (game root, log, memory helpers) into
  `MGSPWEnabler.asi` unchanged. Those sources are written against the `MGS4E_*` identity
  macros; `pw\src\version.hpp` shadows `shared\version.hpp` for this project only and
  re-points the identity names at the Peace Walker values (`shared\pw\identity.hpp`). The
  version numbers are the shared ones. Nothing under `asi\` is edited for Peace Walker.
- **The namespace of the shared core stays `mgs4e::`**; Peace Walker code is `mgspwe::`.
  Renaming the core would touch every MGS4 file for no gain.

The private aspect-ratio work lives in the MGS4 ultrawide module, `external\ultrawide`, under
`pw\`, with the same credit-required licence; the public tree does not describe how it works
and carries none of its values. The research lab is public: [lab.md](lab.md).
