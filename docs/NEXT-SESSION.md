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
- **Research keys only work in lab mode, and lab mode only exists in a Lab build.** They are read
  from `MGS4Enabler.lab.settings`, and only when a fresh `MGS4Enabler.lab` marker sits beside it;
  the ASI consumes the marker. Putting them in `MGS4Enabler.settings` does nothing. Since
  2026-09-04 the instrumentation is compiled only into the **Lab** configuration
  (`build\build.cmd lab` -> `bin\Lab`): every research switch is declared with
  `MGS4E_LAB_SWITCH` (`shared/lab.hpp`) and is a constexpr `false` in a Release build, so the
  probes, scans, dumps, hotkeys, stage automation, the marker check and all their strings drop
  out of the shipped ASI (5015 -> 2460 printable strings; verified by grepping the binary). The
  harness deploys `bin\Lab` on `-LabConfig` and `bin\Release` otherwise; `run_test.ps1` always
  uses the Lab build; `package.ps1` refuses a Lab ASI. The F11 HUD report (Debug Logging) is the
  one piece of diagnostic output still in Release, by design: users report misplaced widgets
  with it.
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
- **The game runs D3D11 by default** (mgs4.ecf: pi = dx11; the options menu offers both).
  The user picks the API in the game's launcher; it is stored under mgs4_savedata_win, which is
  Steam Cloud synced and must never be edited by us. Engine-side hooks are API-agnostic; the
  masking bands and the anisotropy sampler hook have a D3D11 path and a D3D12 path each
  (AR-012 in the private letterbox register). Verify D3D11 by the log line D3D11 device created.
- **The engine picks the largest display mode and, under DX12, switches the display to it.**
  Start-up walks EnumDisplaySettingsW over every mode and keeps the biggest; that is its window
  size and the fullscreen mode. With the window override on, InstallDisplayModeClamp
  (render_pipeline.cpp) answers the walk with the current mode only, so the game stays on the
  desktop's mode. Log line: display mode list is answered with the current mode only.
- **The engine never scales its picture.** Fullscreen is a borderless desktop-size window and
  the final blit is windowSize pixels at the origin (D3D11 census: bind back buffer, viewport
  (0,0,windowSize), one triangle). The Window Aspect Ratio override therefore needs the fit
  (AR-014 in the private letterbox register): viewports and scissors remapped while the back
  buffer is bound, D3D11 and D3D12 paths. Verify by the MGS4: fit: log line.
- **The renderer squeezes any non-16:9 window's picture into a centred 16:9 band** (AR-015
  in the private letterbox register), on D3D12 and D3D11 alike: its last offscreen pass uses
  that band as its viewport, letterbox at 4:3 and pillarbox at 21:9, scene and HUD together,
  so everything comes out squeezed with uncleared margins. The fit hooks on both backends
  widen that band to the window (`Band Expand` lab key). Log line: the engine's 16:9 band ...
  is widened. Two wrong icon crops can look right when two errors cancel - the Stretched HUD
  looked round under DX12 for exactly that reason - and a masked menu hides the band entirely,
  which is how D3D11 passed for a day. Read the whole picture, in gameplay, not one icon.
- **Under DX12 the engine's composition targets are chain-sized while its viewports are
  window-sized** (AR-015, second part): with the override on, the picture landed 1:1 in the
  top-left of the chain and the back-buffer blit copied it as is. The D3D12 fit therefore
  applies to viewports on chain-sized offscreen targets too (RTV sizes tracked from
  CreateRenderTargetView), and the chain outside the fitted rect is cleared every present.
  D3D11 sizes those targets by the window, so its back-buffer fit alone was right.
- **A perf census lives in the Lab build.** With Debug Logging on, a Lab ASI logs one MGS4: perf:
  line a second: presents, frame ms avg/p50/p99/max, and the time spent in each of our per-frame
  hooks (pool append, upload probe, layout converter, bands). Compare two sessions of the same
  scene before theorising about stutter; it found AR-013 (VirtualQuery per menu node, 0.4 ms
  each) in one pair of runs. The summariser is ad hoc (scratchpad perf/sum.sh), rewrite as needed.
- **Other mods' config files get a tab in the tool when present** (	ool/src/mod_config.*): a table of
  known files (MGSFPSUnlock.ini first) with the keys to expose; Detect looks beside mgs4.exe
  (MGS4\, MGS4\scripts, plugins, update) and, flagged not-loadable, the same names under the
  install folder. A save rewrites only the listed values in place. The compat table is a
  different thing: settings both mods control. MGS4Enabler.exe --tab <title> opens on a tab,
  for screenshots.
- **Other mods' settings files are never written by the tool or the ASI** — they are read for
  the compat table and the one-time import, nothing else. The user's own settings live in
  `MGS4Enabler.settings`; leave that file alone unless they ask.
