# Start here

**MGS4 SSAA and Aspect Ratio Enabler** is a standalone graphics mod for MGS4 in the Master
Collection. It has its own ASI (`MGS4Enabler.asi`), its own settings file
(`MGS4Enabler.settings`) and its own wxWidgets tool (`MGS4Enabler.exe`); no code from any
other mod is compiled in. Mod compatibility (`shared/compat_table.hpp`) lists the mods it
knows how to share the game with and reads their settings files read-only to stand aside
where both patch the same thing; that table is the one place another mod is named by design.
The project name and short id (`MGS4Enabler`) date from 2026-09-04.

**Next task: aspect ratio fixes — ultrawide (21:9, 32:9) and 4:3.** The open items are the
README's "Known issues" list (2026-09-06): the in-game options menu over the Nomad interior
(same shape as the codec portrait, AR-016, or the sub-canvas mapping, AR-018), the 3D item,
weapon and camo renders, the map render, and subtitles in gameplay and cutscenes.

The aspect-ratio implementation, its bug register and its working notes live in the private
module at `external/ultrawide` (a submodule; see its README). Without access to it the project
still builds, with a stub in place of the fixes. Begin at that module's docs. The gameplay HUD
is done at 21:9 and 4:3, and cutscene/menu masking plus the FOV settings shipped in the fork's
0.2.6; what remains is listed under "Remaining work" in that module's `aspect-ratio.md` and in
`letterbox.md` §5.

**Releases are packaged locally** with `build\package.ps1` (7-Zip, needs the Ultimate ASI
Loader zip). Do not prepare a release unless asked. Every version bump gets its entry in
`CHANGELOG.md` first (Nexus-ready plain text: Fixes / New / Notes); the packager refuses a
version with no entry.

**Never push or tag without being asked.** Commit freely; pushing is the user's call every time.

---

## To do

Kept here so nothing lives only in a conversation. Add to it; strike through when done.

- **DirectX 11 run of the 16:10-on-21:9 case** (AR-020 in the private letterbox register).
  The band matchers are shared with DirectX 11 and the same fractional scale applies, but only
  DirectX 12 was booted. Native 3440x1440 desktop, 16:10 override 2560x1600 (capped to
  2304x1440), menu and gameplay captures, user's read.
- **Config tool crash, 2026-09-11 02:29.** Windows Error Reporting: `MGS4Enabler.exe` 0.0.6,
  exception 0xc0000005, `PCH_2B_FROM_ntdll+0x161914`, no faulting module named (event log,
  Application, ids 1000 and 1001; a WER dump was written under
  `C:\ProgramData\Microsoft\Windows\WER\Temp`). The user has not said what they were doing in
  the tool at the time; ask first, then reproduce with the current build.
- **Still open from the README's known issues** (all ultrawide / 4:3, none at 16:9): the
  in-game options menu over the Nomad interior; the 3D item, weapon and camo renders; the map
  render; subtitles in gameplay and cutscenes.
- **Never verified**: 32:9 (no mode on the capture output), a real play session at 21:9, the
  letterbox "not done" list in the private module's `letterbox.md` §5.

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
- **The engine keeps its resolution twice** (WR-002 in window-resolution.md): the window-size
  globals the override writes, and the renderer context's init record (`context+0x4C1230C`),
  which the per-frame submit at `+76DFF0` copies into each frame and the backends size their
  buffers, clamp their view rects and request the chain resize from. The override now writes
  both (`InstallRendererResolution`). What the fullscreen resize "asks for" is that second copy,
  the game's own resolution setting, which defaults to the largest display mode.
- **The engine never scales its picture.** Fullscreen is a borderless desktop-size window and
  the final blit is windowSize pixels at the origin (D3D11 census: bind back buffer, viewport
  (0,0,windowSize), one triangle). The Window Aspect Ratio override therefore needs the fit
  (AR-014 in the private letterbox register): viewports and scissors remapped while the back
  buffer is bound, D3D11 and D3D12 paths. Verify by the MGS4: fit: log line.
- **FusionFix's shaders under DirectX 12** (`docs/features/shader-replacement.md`,
  `asi/src/features/shader_port.*`, `shaders/ReplacedShadersPS.dx12.map`): the DirectX 12
  shader build is the DirectX 11 one with larger constant buffers (indices shifted by the
  size difference) and narrower input masks; replacements are ported at pipeline creation.
  Lab keys `Dump Pixel Shaders`, `D3D12 Debug Layer`; the scratchpad scripts that built the
  map (match/port/binport) are described in the doc.
- **Features, one owner each** (`shared/features.hpp`, `shared/manifest_claims.hpp`): manifest
  blocks carry `Feature`/`Off`/`AlwaysOn`; our own claims are `features::kOurs`. The tool
  (`ActiveOwners`/`ResolveOverlaps` in ui.cpp) annotates rows and asks at save; the ASI
  (`compat::Yields` + `LogOverlaps`) stands down and warns. Manifests for the unlocker and
  FusionFix ship in the zip; PatriotFix's lives only in the user's game folder.
- **DirectX version and window mode are overridable in-process** (`docs/features/display-mode.md`):
  the saved-settings loader at +65D880 writes the store's `render.api` and copies the mode
  word to +3BD1150; the boot code creates the window through the mode getter +65D7B0 before
  that loader returns. The override wraps the loader (store string and bool rewritten, word
  written) and hooks the getter. Engine modes: 0 full_exclusive (popup, topmost), 1
  full_borderless, 2 windowed. Nothing writes mgs4.savedsettings, but the game saves the
  running API there itself on exit. Lab key `Find References` lists rip-relative and
  call/jmp references to an RVA.
- **The renderer squeezes any non-16:9 window's picture into a centred 16:9 band** (AR-015
  in the private letterbox register), on D3D12 and D3D11 alike: its last offscreen pass uses
  that band as its viewport, letterbox at 4:3 and pillarbox at 21:9, scene and HUD together,
  so everything comes out squeezed with uncleared margins. The fit hooks on both backends
  widen that band to the window (`Band Expand` lab key). Log line: the engine's 16:9 band ...
  is widened. The band is the **chain's** 16:9 band scaled into the window rect, not the
  window's own (AR-015 fifth part): a 16:10 override on a 4:3 display got (0,125 1600x750),
  the 1600x1200 chain's (0,150 1600x900) times 1000/1200; the matcher builds it that way now.
  A 16:9 chain has no band, which is why every override on a 16:9 desktop was fine. Two wrong icon crops can look right when two errors cancel - the Stretched HUD
  looked round under DX12 for exactly that reason - and a masked menu hides the band entirely,
  which is how D3D11 passed for a day. Read the whole picture, in gameplay, not one icon.
- **Codec calls render the caller with a second camera whose projection follows the window's
  aspect** (AR-016 in the private letterbox register, issue #1): m00 fixed, m11 = window aspect
  x 1.0171, squeezed into a 16:9 pane. The projection setter hook holds m11 at its 16:9 value
  while a screen is live; the masking bands stay up through the call (dropping them at the
  connect was a visible jump, user's read). Lab key
  `Codec Portrait Fix`. To reach a call in the harness: stage boot, Tab, Enter (CODEC is the
  first pause entry), then a *left click* for SEND - synthetic Enter is ignored on that screen.
  Never press Up/Down there (manual tuning); Left/Right pick other contacts.
- **The layout converter also gets sub-canvas calls** (AR-018 in the private aspect-ratio
  register): the system menus (difficulty, load list, options) lay out sub-rects such as
  `(100,87 1180x532)`, which the game maps with two independent scales, so they stretched over
  the full window. The hook now places a sub-rect's physical rect under the same uniform scale
  and logical offset the full-canvas fix uses. The gameplay census that said "86 of 86 calls
  are full-canvas" was true only of gameplay.
- **Pre-rendered videos are one native full-surface rect** (AR-017 in the private letterbox
  register): emitted from +E367BB through the rect emitter at +BE090, so they take the window's
  shape. With the HUD fix on that rect becomes the centred 16:9 rect of the surface. The
  prologue drama stages are engine cutscenes; the videos are the NEW GAME opening (Up twice at
  the main menu, Enter, Enter, Enter in the harness).
- **Under DX12 the engine's composition targets are chain-sized while its viewports are
  window-sized** (AR-015, second part): with the override on, the picture landed 1:1 in the
  top-left of the chain and the back-buffer blit copied it as is. The D3D12 fit therefore
  applies to viewports on chain-sized offscreen targets too (RTV sizes tracked from
  CreateRenderTargetView), and the chain outside the fitted rect is cleared every present.
  D3D11 sizes those targets by the window, so its back-buffer fit alone was right.
- **Windowed mode stutters and it is the game, not us** (2026-09-07). The user saw 100-500 ms
  frames in windowed mode only, with fullscreen 1 % lows above 100 fps on the same scene. The
  Lab perf census in windowed mode (2880x1800 window, 200 % internal scale, 240 fps target):
  median frame 4.2 ms, a recurring ~20 ms p99, our hooks under 9 ms per second in total and 0.1
  ms at the worst single call, no per-present GPU work of ours in gameplay. Turning the game's
  own V-Sync off was "a big improvement, but nowhere near fullscreen"; the user's read is the
  compositor. README carries the note. Do not chase this as a mod bug without a census that
  puts the time inside a hook.
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
