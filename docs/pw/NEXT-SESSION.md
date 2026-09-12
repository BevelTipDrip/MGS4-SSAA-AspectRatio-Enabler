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

- **PW-HUD-001 lens flare at high resolution** (user report 2026-09-12, at 6880x2880 through
  Afevis's ini): the flare appears at random and its anchor lands off screen, as if a value
  overflows. Lead: the half-glare site Afevis patches begins with a sign-extended 16-bit move,
  so the anchor is int16 somewhere before it becomes a float; a pixel-space step at render
  scale 10.59 could pass 32767. Plan: census on a hotkey (press when it misbehaves), probes on
  the glare and half-glare sites logging the registers and the 16-bit anchor, compare
  3440x1440 against 6880x2880.
- Record the title-to-menu route; census that menu; then the (texture, rectangle) bias
  command for live editing.
- A title census with Afevis's ASI removed from `mgspw\scripts` (what of the picture is his).
- Phase 0, in order (the device, swap chain and UI-space items are answered).
- `build\package.ps1` stages the Peace Walker files into the combined zip (done in code, not
  run: the user says when to package).
- MGS4 regression boot after the shared-header change (`boot_to_aim.ps1 -Deploy -StopAtMenu`
  against the baseline log in the session scratchpad); waits for the user to be off the game.
