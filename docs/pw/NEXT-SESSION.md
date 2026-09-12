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

## To do

- Phase 0, in order.
- `build\package.ps1` stages the Peace Walker files into the combined zip (done in code, not
  run: the user says when to package).
- MGS4 regression boot after the shared-header change (`boot_to_aim.ps1 -Deploy -StopAtMenu`
  against the baseline log in the session scratchpad); waits for the user to be off the game.
