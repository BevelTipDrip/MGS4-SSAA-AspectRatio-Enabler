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

## State

Research. The ASI loads, reads its settings and logs, and patches nothing yet. What is known
about the engine so far is in [engine-research.md](engine-research.md); the plan of work is in
[NEXT-SESSION.md](NEXT-SESSION.md).

## For the code

Two rules that are not obvious from the tree:

- **The shared core is compiled from `asi\src\core`** (game root, log, memory helpers) into
  `MGSPWEnabler.asi` unchanged. Those sources are written against the `MGS4E_*` identity
  macros; `pw\src\version.hpp` shadows `shared\version.hpp` for this project only and
  re-points the identity names at the Peace Walker values (`shared\pw\identity.hpp`). The
  version numbers are the shared ones. Nothing under `asi\` is edited for Peace Walker.
- **The namespace of the shared core stays `mgs4e::`**; Peace Walker code is `mgspwe::`.
  Renaming the core would touch every MGS4 file for no gain.

The private aspect-ratio module, when it exists, is `external\ultrawide-pw`, under the same
credit-required licence as the MGS4 one; the public tree does not describe how it works.
