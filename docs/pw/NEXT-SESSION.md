# Peace Walker: start here

Read the root `docs/NEXT-SESSION.md` first for the conventions (evidence levels, confirm every
screenshot with the user, close the game after every test, never push). This file is the
Peace Walker-specific state.

## Where things are

- Game: `C:\Program Files\Steam\steamapps\common\MGS_PW`, exe `mgspw\METAL GEAR SOLID PEACE
  WALKER.exe` (spaces in the name; every script quotes it), Steam app 2492660, saves in
  `mgspw_savedata_win` beside the install.
- Our build: `pw\` project, `bin\<cfg>\MGSPWEnabler.asi`, deployed to `mgspw\scripts\`.
  Lab: `MGSPWEnabler.lab` marker + `MGSPWEnabler.lab.settings` in the install root, same
  protocol as MGS4's. Lab keys so far: `Log Loaded Modules`, `Log Decrypt Timing`, `Log Command
  Line` (`pw\src\features\probe.cpp`).
- Harness: `C:\mgspf_tools\pw\` (`game.ps1` holds every constant; `boot.ps1 -Deploy
  -LabConfig`, `close_game.ps1`, `shot.ps1`, `send_input.ps1`, `textdump.ps1`). No stage
  automation, no gameplay detector yet.
- Other mods in the game folder for the user's own tests (installed 2026-09-12): Afevis's
  `MGSPWResolutionUnlocked.asi` (MIT, source in `..\Afevis\`, log
  `logs\MGSPWResolutionUnlocked.log`) and MGSPatriotFix 0.2.2 (`mgspw\scripts\` and the
  launcher half in `launcher\scripts\`). Their presence is expected in our compat log.

## Phase 0: engine research (do this before any feature)

The ordered list is Part 7 of the approved plan; in short: vanilla-run facts and baseline
captures at each desktop shape (user reads them); loader + probe boot (modules loaded, decrypt
timing, command line); live `.text`/`.rdata` dump at the title; window and mode selection
sites; device and swap chain (both DirectX 11 and 12 hooked, queue taken from every chain);
internal render size and the upscale point; shader pipeline (11 `.cso` files, D3DReflect at
run time); FOV/aspect/UI space (480x272 constants). Write `engine-research.md` with evidence
levels; only then design `pw\src\features`.

Afevis's tool is the map of the resolution plumbing (28 pattern hooks: preset tables,
render-target creation and getters, render scale, FOV and culling FOV, a dozen HUD element
fixups); the launcher drives the game with `-resolution 0|1 -upscale 0..3 -movie 0|1`.

## Where the UI work stands (2026-09-13)

The Lab draw census works on Peace Walker's deferred-context renderer (`lab.md`). The UI
method, the element tables and the live-editing biases are in the private module
(`external/ultrawide/pw/docs/`); the mission HUD groups were moved to the 21:9 edges and the
user confirmed the result. Route replay: `boot.ps1 -Deploy -LabConfig -KeepOpen -WaitSeconds 1`,
then `replay.ps1 -Route routes/to_mission.csv -SkipMs 15000 -StopMs 58000` lands on the Mission
Selector; `-StopMs 108000` lands in the mission. `census 2` goes into `live.txt`; the
live-editing commands come from the private module. `bias_run.ps1` is one run end to end.

## Release shape (user, 2026-09-13)

The Lab tabs stay while features are built and tested. For the release the Peace Walker window
gets the MGS4 layout with the Peace Walker options: Enable logging; Render resolution (the
integer canvas scale); screen / window resolution and window mode; aspect ratio selection;
MSAA; and every graphics option the game's own settings file offers (the persisted settings
array the launcher and the display code read: window mode, position, size, monitor, windowed
preset, and the rest once enumerated). The Release ASI reads those as normal settings; the
research keys remain Lab-only.

## To do

- **Write-through Map fallback for GPU-local frame textures** (investigation path, tabled
  2026-09-13): if a scene ever maps one of the frame textures from the CPU (the log reports
  "Map on a GPU-local texture"), answer the Map with our own scratch buffer and upload it at
  Unmap, so the texture stays in video memory and the game keeps its CPU write. Writes only;
  persistent scratch per texture if the game expects to see previous contents.

- ~~**PW-HUD-001 lens flare at high resolution**~~ **closed 2026-09-14 (user): his patch only.**
  The random flare with its anchor off screen (reported 2026-09-12 at 6880x2880 through Afevis's
  ini) does not happen under our patch at any scale: every full-screen effect keeps working, as
  the elimination runs of 2026-09-13 predicted (his canvas widening and its per-effect fixups
  are the cause; ours never widens the canvas at 16:9 and widens it consistently above it).
- Record the title-to-menu route; census that menu; then the (texture, rectangle) bias
  command for live editing.
- A title census with Afevis's ASI removed from `mgspw\scripts` (what of the picture is his).
- Phase 0, in order (the device, swap chain and UI-space items are answered).
- `build\package.ps1` stages the Peace Walker files into the combined zip (done in code, not
  run: the user says when to package).
- MGS4 regression boot after the shared-header change (`boot_to_aim.ps1 -Deploy -StopAtMenu`
  against the baseline log in the session scratchpad); waits for the user to be off the game.

## 2026-09-14: two testing traps (open)

- **Use Display on a 16:9 desktop builds a 484x272 canvas.** The start-up resolver
  (`InternalSize::Apply`) compares the display aspect (3840x2160 = 1.7778) with the 480x272 canvas
  (1.7647), calls it wider, and makes a 484-unit canvas with a 2-unit UI offset: gaps beside the
  menu elements. Selecting "16:9" explicitly gives the game's own 480x272 (the stock 0.7 %
  vertical squeeze into a 16:9 picture). **Decision (user, 2026-09-14): leave the code alone for
  now; later Use Display will scale the same way the explicit shapes do**, i.e. resolve the
  measured display shape to the same canvas the matching list entry would give (a 16:9 display to
  480x272 and its stock squeeze), rather than building a bespoke canvas per measured aspect. Until
  that is done, test with an explicit shape and never Use Display on a 16:9 desktop.
- **The harness's lab settings file must mirror the user's settings.** `boot.ps1 -LabConfig`
  makes the ASI read `MGSPWEnabler.lab.settings`; a stale copy of that file (old Internal Size
  3840x2160 keys, Use Display, Render Scale 16 with 8x resolutions) produced a run with no
  supersampling and the 484 canvas, which the user had to diagnose from the picture. Before a Lab
  launch, regenerate the lab file from `MGSPWEnabler.settings` and change only the Lab keys under
  test. Check in with the user before launching and before closing while they are live testing.

## 2026-09-14: the 60 Hz stutter (root cause found; residual open)

Full write-up: [frame-pacing.md](frame-pacing.md). Short form: the doubled frame once a second
in Borderless and exclusive Fullscreen at 60 Hz (composed windows are smooth, 120 Hz is smooth,
the stock game does it too) is the game's own frame-skip governor at `+78462`: a frame that
measures a fraction over a vblank by wall clock raises the vblank wait count, so the next frame
waits two vblanks. Direct presentation lands frame ends on the vblank boundary, so it trips 2-4
times a second. `Frame Skip Governor = false` (Lab) skips that store; the user judged it
"significantly better". Not yet as smooth as 120 Hz: the game thread still misses ~1.3 ticks a
second with its work at 16.8-17.2 ms, cause unknown (D3D stall on the game thread, or CPU work,
or idle GPU clocks).

**Pick up here:**
1. One run, governor off, user presses F10 in gameplay: the per-miss lines say where the 17 ms go
   (D3D call, wait, lock, or plain work). Then fix that.
2. Promote `Frame Skip Governor` into the Release ASI as a shipped setting (default: the fix on),
   with the tool field on the Graphics tab; keep the Lab knobs as they are.
3. Retest, without `Pacing Log`, anything judged under the heavy instrumentation if it becomes
   relevant again (Swap Chain Buffers 3, Allow Tearing, VBlank ticker).
4. Optional user-side check: NVIDIA "Prefer maximum performance" for the exe (idle GPU clocks).

## 2026-09-12 late: the 21:9 canvas works (Lab)

Render Scale 10, Wide Canvas Units 650, Output 3440x1440, Window Mode 0 on a 3440x1440
desktop: world at 21:9, HUD and menus at the 16:9 scale and centred, HUD groups at the edges
(the register's biases), full-screen effects across the frame, no flicker. The method and the
sites are in the private module (external/ultrawide/pw: wide_canvas.cpp, engine-notes.md).

**Closed 2026-09-14 (user):** the "slightly darker" impression at 21:9 was wrong; A/B
screenshots against 16:9 show the same picture. **16:10 is tested and working** (the tall-canvas
path, 480x300 units, 14 units of vertical move per side), alongside the 21:9 and 4:3 runs already
confirmed. So every shape the Aspect Ratio list offers has now been seen working except 32:9,
which no display here can show.

Next:
- Bake the thirteen HUD moves into the shipped fix (private aspect_ratio.cpp) instead of the
  live `tbias` commands; table the pause menu, Codec and overlays.
- The Window Mode knob and the saved-display-mode log belong in the Release feature set.

## 2026-09-13: exclusive fullscreen stutter (fixed in the Lab build)

The game hard-codes 60/1 as the refresh rate of its swap chain description. In exclusive
fullscreen on a 120 Hz display the chain landed on a 60 Hz mode and the game dropped exactly one
frame a second (59 frames per second, a 33 ms frame every 50; the census's hitch log showed no
resource creation, no waiting maps and no log lines in those frames, and Light Hooks made no
difference; Windowed and Borderless were clean). The census's CreateSwapChain hook now replaces
the requested rate with the display's current one for exclusive chains (3840x2160 @ 120 Hz on
the user's desktop): 60 frames per second, no stutter, user-confirmed. The hitch log (a warning
per frame over twice the running average, with what happened inside it) and the chain's actual
mode line stay in the Lab build. For Release this belongs in the Window Mode feature.

## 2026-09-13: anisotropic filtering (Lab option, verified)

The game creates no anisotropic sampler: in a mission every sampler is MIN_MAG_MIP_LINEAR,
MIN_MAG_LINEAR_MIP_POINT or POINT with MaxAnisotropy 0 or 1 (census sampler log, option off).
The Anisotropic Filtering option (2..16) makes every linear, non-comparison sampler anisotropic
at that level at creation; point samplers stay as they are. 16x confirmed by the user with no
visible side effects. Note: the game reads created samplers' descriptions back into its
template, so with the option on later creations already carry the filter; judge the game's own
use only with the option off.
