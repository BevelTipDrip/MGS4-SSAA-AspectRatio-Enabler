# Start here

**MGS4 SSAA and Aspect Ratio Enabler** is the standalone home of the MGS4 graphics work that
earlier shipped inside a fork of another mod. It has its own ASI (`MGS4Enabler.asi`), its own
settings file (`MGS4Enabler.settings`) and its own wxWidgets tool (`MGS4Enabler.exe`); no code
from any other mod is compiled in. Mod compatibility (`shared/compat_table.hpp`) lists the
mods it knows how to share the game with and reads their settings files read-only to stand
aside where both patch the same thing. The old fork repository (`..\repo`) is frozen at its
0.2.6 release. The project name and short id (`MGS4Enabler`) date from 2026-09-04; the earlier
working name was "PF Companion", and the user wants no reference to the other mod in the name
or the wording beyond the compatibility feature itself.

**Next task: aspect ratio fixes — ultrawide (21:9, 32:9) and 4:3.**

The aspect-ratio implementation, its bug register and its working notes live in the private
module at `external/ultrawide` (a submodule; see its README). Without access to it the project
still builds, with a stub in place of the fixes. Begin at that module's docs. The gameplay HUD
is done at 21:9 and 4:3, and cutscene/menu masking plus the FOV settings shipped in the fork's
0.2.6; what remains is listed under "Remaining work" in that module's `aspect-ratio.md` and in
`letterbox.md` §5.

**Releases are packaged locally** with `build\package.ps1` (7-Zip, needs the Ultimate ASI
Loader zip). Do not prepare a release unless asked.

**Never push or tag without being asked.** Commit freely; pushing is the user's call every time.

---

## Layout

```
shared/          version, settings keys, INI reader, compat table — used by both projects
asi/src/core/    game (module/root), mem (scanning), log, config (settings + lab mode), compat
asi/src/features/render_pipeline, graphics_settings, stage_automation, aspect_ratio (stub)
tool/src/        fields (the settings table + help), settings_io, compat, ui, main
external/        safetyhook, zydis, spdlog, wxWidgets, ultrawide (private)
build/           build.cmd, build_zydis.cmd, build_wx.cmd, package.ps1, make_art.ps1
```

Game side: `MGS4\scripts\MGS4Enabler.asi` (the harness `-Deploy` copies there too — one
copy only, the loader also loads from `MGS4\` and two copies would both hook), settings and
tool in the game root, log at `logs\MGS4Enabler_Game.log`. Never put an `.asi` in the game
root.

---

## Read these, in this order

Reading order matters: each one assumes the engine facts established by the one above it.

| # | Document | Why it is needed for this task |
| --- | --- | --- |
| 1 | [features/README.md](features/README.md) | evidence levels, bug entry format, the conventions every claim here is held to |
| 2 | `external/ultrawide/docs/features/aspect-ratio.md` | the task itself — known facts, open questions, design constraints (private module) |
| 3 | [features/window-resolution.md](features/window-resolution.md) | how the window size is already overridden, and **WR-001**, the one existing claim in this area |
| 4 | [features/supersampling.md](features/supersampling.md) | the render-buffer vs output-size distinction, and **SS-003** — what the removed wrap-around UI patch broke |
| 5 | [features/tooling.md](features/tooling.md) | lab mode, the test harness and the per-draw UI diagnostics needed to inspect a frame |
| 6 | [features/anti-aliasing.md](features/anti-aliasing.md) | only for its "limits of the automated check" section — why screen-measurement metrics here are not trustworthy |

Also worth having open: `asi/src/features/render_pipeline.cpp` for the UI research
diagnostics, and `asi/src/features/graphics_settings.cpp` for how a shipping patch is structured
(including how it stands aside for another mod via `compat::Yields`).

---

## Facts that will save a day each

These are all written up in the documents above, but they are the ones most likely to be
rediscovered the hard way:

- **Read the render size from the log, never the settings file.** With `Window Aspect Ratio` at
  `Use Game Setting`, the resolution keys are not read at all and the game runs at the display
  resolution. A whole set of measurements was mislabelled this way — self-consistent, and wrong.
- **The UI uses fixed virtual coordinate spaces** (1280x720, 1429x800, 720x400), none of them
  aspect-derived. This is why a non-16:9 window is expected to distort rather than reveal.
- **`.text` is encrypted on disk** by the Steam DRM, so all code reading must happen in-process.
  Static disassembly of the exe shows nothing useful.
- **Research keys only work in lab mode.** They are read from `MGS4Enabler.lab.settings`, and
  only when a fresh `MGS4Enabler.lab` marker sits beside it; the ASI consumes the marker. Putting
  them in `MGS4Enabler.settings` does nothing.
- **`Log Viewports` and `Watch Staging Writes` make the game unplayable.** Use them for a captured
  frame, never for a play session. Left on once, the stutter was mistaken for a rendering
  regression.
- **A screensaver breaks automation in a way that looks like a crash.** It switches the input
  desktop to `Screen-saver`, after which input never reaches the game and screen capture throws
  "the handle is invalid" — while the game keeps running fine. `boot_to_aim.ps1` now detects this
  and exits 4; `screensaver_guard.ps1` prevents it.
- **`CopyFromScreen` can fail from a non-interactive shell** with the same "handle is invalid"
  even with the desktop unlocked; `PrintWindow` with `PW_RENDERFULLCONTENT` into a bitmap you own
  works from anywhere and is how the tool's window is captured.
- **Mod compatibility is one table.** `compat_table.hpp` lists known mods (`kMods`) and the
  colliding settings (`kTable`); a mod that sorts before us in `MGS4\scripts` loads first and
  wins. Entry 0 is MGSPatriotFix (aniso: whenever its ASI is present; shadow resolution: when
  its `Custom Shadow Resolution` is non-zero). Both the ASI and the tool decide from this
  table; do not add a second source of truth. Adding a mod = a `Mod` row plus its `Entry`
  rows; the log, tool note, greyed labels and README table all follow. Verified 2026-09-04
  against MGSPatriotFix 0.2.1 built from the fork repo's `upstream` remote
  (`git checkout --detach upstream/master`, build with `/p:PreBuildEventUseInBuild=false
  /p:PostBuildEventUseInBuild=false` — its build events break on the spaced path and its
  post-build copies into the game folder).
- **MGSPatriotFix's ASI aborts on a settings file missing any of its keys** (`FatalConfigError`
  → `FreeLibraryAndExitThread`), and nothing after it in `MGS4\scripts` loads — so our ASI
  never logs and the run looks like a hang. A fork-era `MGSPatriotFix.settings` is exactly such
  a file; run their Config Tool once (it rewrites the file with only its keys). Their tool's
  buttons are `Save and Exit` / `Exit` / `Launch Game` — there is no plain `Save`.
- **The harness gates on `Initialisation complete` outside lab mode.** Stage automation only
  exists in lab mode, so `-LoadSave` and the `-StopAt*` paths work on a normal boot; a stage
  request needs `-LabConfig` with `Stage Automation=1` (and `Stage Automation Stage` for a
  specific stage). `close_game.ps1` closes a game left up by a `-StopAt*` run.
- **The gameplay detector fails when the game does not fill its client area** (PiP monitor
  modes, a 1920x1080 override drawn into the top-left of a 3840x2160 client): `in_game.ps1`
  reads fractional regions of the capture, so the HUD is outside its window. The captures are
  still fine; the log and the user's read are the evidence in that situation.

---

## How to work on this

- **Confirm before concluding.** Send any screenshot to the user and wait for their read before
  treating it as fact. Pixel-counting detectors return a number, not a fact.
- **When something unexpected happens, ask what they saw** rather than theorising. Four
  consecutive wrong crash theories in one session came from skipping this; the user's original
  explanation was correct.
- **Close the game after every test.** `boot_to_aim.ps1` does this on all exit paths, but a
  manual launch is yours to clean up.
- **Evidence levels are enforced.** Inferred is a hypothesis with a note attached, not a reason to
  change code. Promote to Observed or Measured first, or record that it could not be promoted.
- **Other mods' settings files are never written by the tool or the ASI** — they are read for
  the compat table and the one-time import, nothing else. The user's own settings live in
  `MGS4Enabler.settings`; leave that file alone unless they ask.
