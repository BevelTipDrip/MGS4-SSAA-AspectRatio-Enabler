# Peace Walker: start here

Read the root `docs/NEXT-SESSION.md` first for the conventions (evidence levels, confirm every
screenshot with the user, close the game after every test, never push). This file is the
Peace Walker-specific state.

## START HERE: pacing is solved and shipped; what is owed is a soak (2026-09-17)

Three fixes are in the Release build, each proven by measurement and confirmed in play by the user.
Full detail and every dead end: `docs/pw/frame-pacing.md`.

| Fix | Module | What it removed |
| --- | --- | --- |
| Busy Wait Fix (level 3) | `busy_wait.cpp` | three spin loops; process CPU 122% -> 24% of a core; the 60 Hz stutter |
| State Object Cache | `state_cache.cpp` | 350 redundant device state creations a frame, ~8% of game-thread CPU |
| Audio Prefetch (voice archives at start) | `file_prefetch.cpp` | the single-frame hitch when a voice line or Codec call starts: a ~10 ms cold archive read on the game thread |

Also fixed: `ContextHooks::original` was sized 80 while slots 111 and 114 were stored into it, an
out-of-bounds write that corrupted whichever globals followed. It was Lab-only (Release's highest
slot is 57) but it explained two sessions of symptoms that moved with code layout. Lesson, recorded
in memory: **when a fault moves with layout, attach a debugger; do not add instruments.**

**Owed, not done:** a soak with all three on through cutscenes and movies (paced to 30 by design at
`+78070`; do not mistake that for stutter), Mother Base, a mission end, the three window modes and
alt-tab; and a look at the start-up warm on a 16 GB machine, since 1.6 GB of page cache is
reclaimable but not free. The Config Tool exposes all three on the Peace Walker page under
Performance. The preview packager (`build\package_pw_preview.ps1`) ships plugin, loader, tool and
template together.

### The earlier plan, kept for the record (2026-09-15)
#### START HERE: the 60 Hz stutter is solved; the work left is promotion (2026-09-15)

Full detail and every measurement is in `docs/pw/frame-pacing.md`. This is the state and the plan.

### What the answer turned out to be

**The game never blocks.** It spins in three places where it should wait, and together they cost
about a full core. Fixing the first one gave the user a flat 16.6 ms frame time at a solid 60, on an
otherwise stock frame loop. Their words: "It ran incredibly smoothly, that was it all along."

| Loop | Site | The defect |
| --- | --- | --- |
| Render thread | loop `+17BD0`, sleep at `+17CCF` | reads the handoff byte at display`+0x32c0`; with no frame ready calls `Sleep(0)` and reads it again, forever |
| Ticker | loop `+76020`, sleep at `+76051` | truncates the time left to whole milliseconds, so the last fraction spins; and calls `timeBeginPeriod`/`timeEndPeriod`, a system-global lock, **every iteration** |
| Message pump | `+77610` | `PeekMessageA` then `Sleep(1)`, which no arriving message can wake |

Process CPU, switched live in one spot with nothing else changed:

| Level | What is on | Process CPU |
| --- | --- | --- |
| 0 | the game's own spins | 122.1% of one core |
| 1 | render thread event | 26.8% |
| 2 | + ticker timer | 36.9% (menu noise; its own thread was only 3-4%) |
| 3 | + message pump | 23.6% |

Counters confirm the mechanism: per 5 s the render thread's waits are satisfied by the event
**exactly 300 times**, which is 60 a second, one per frame. At level 3 the harmless 2 ms timeouts
fall from ~2200 to ~150 per 5 s, the ticker sleeps 300 times at 15.94 ms, and the pump parks ~1820
times.

### What this retires

- **The frame skip governor is no longer needed for the stutter**, but it stays as a user option
  (user's call 2026-09-16). Note what the knob actually does: it hooks only the **raise** at
  `+78462` and skips that store, so a long-measured frame can no longer promote the wait count. The
  explicit set at `+78070` that paces 30 fps menus and movies, and the lowering path at `+7843C`,
  are both left intact. It turns the governor **down, not off**, and earlier notes calling it a
  disable were wrong.
- **The state object cache is a performance feature, not a stutter fix.** Keep it: it removes 350
  redundant device state creations a frame and about 8% of the game thread's executed CPU, which
  matters on slower processors. It is not load-bearing for pacing.
- Dead ends, all documented with their measurements so they are not retried: releasing the display
  lock across `Present` (crashes in `nvwgf2umx`), waiting for the vblank ourselves (cadence drifts),
  yielding the lock before the flip (works by the numbers, invisible to the eye, and its phase
  reading cost a `GetFrameStatistics` per present), and matching the tick rate to the display (the
  display flips at 59.9998 Hz against a 60.0000 Hz tick, so there is nothing to correct).

### Where the code is, and why that is the problem

Everything built this session lives in **`pw/src/features/draw_census.cpp`**, which the project
compiles **only in the Lab configuration**:

```
<ClCompile Include="src\features\draw_census.cpp" Condition="'$(Configuration)'=='Lab'" />
<ClCompile Include="src\features\render_hooks.cpp" Condition="'$(Configuration)'!='Lab'" />
<ClCompile Include="src\features\render_policy.cpp" />        <!-- both -->
```

and `dllmain.cpp` picks one:

```cpp
#if MGS4E_LAB_BUILD
    Probe::Run();
    DrawCensus::Install();
#else
    RenderHooks::Install();
#endif
```

**So a Release ASI contains none of the fix.** On top of that, `MGS4E_LAB_SWITCH` expands to
`inline constexpr` outside Lab, so `DrawCensus::iBusyWaitFix` is a compile-time `0` in Release and
the whole thing would be dead-stripped even if the file were compiled.

### The promotion plan

**Put it in its own module, not in `render_policy.cpp`.** The fix is self-contained: it needs
`mgs4e::game::Module()`, spdlog, safetyhook, one event and one waitable timer. It touches no
Direct3D and no other feature. So:

1. New `pw/src/features/busy_wait.cpp` / `.hpp`, added to the vcxproj **with no `Condition`** so
   both configurations compile it. Move `InstallBusyWaitHooks`, `Hooked_PeekMessageA`,
   `BusyWaitSleep`, `ReportBusyWait`, the three site constants and the counters across verbatim.
2. The level becomes a **real setting in both builds**: `inline int iBusyWaitFix = 0;` in the new
   header, **not** an `MGS4E_LAB_SWITCH`. Check that the `Read`/`Report` pair for `BusyWaitFix` in
   `pw/src/core/config.cpp` is not inside a Lab-only block; move it out if it is.
3. Call `BusyWait::Install()` from **both** arms of the `#if MGS4E_LAB_BUILD` in `dllmain.cpp`,
   at the same point `DrawCensus::Install()` runs today. That point is proven: the signature check
   passes there, so the game's `.text` is already decrypted.
4. Leave the `busywait` live command in `draw_census.cpp`; have it call a small
   `BusyWait::SetLevel(int)` so Lab keeps live switching and Release does not need the poll thread.
5. Add the field to `tool/src/pw/pw_fields.cpp` so the Config Tool exposes it.

### Challenges to expect, in the order they will bite

1. **The unexplained start-up crash.** Earlier this session the state object cache was implemented
   in `render_policy.cpp` and called from `draw_census.cpp`, and the game aborted at start-up every
   time, **even with the cache disabled**, and even when the only cross-module reference was a
   counter read inside a log line that never executed. Moving the cache into `draw_census.cpp` made
   it go away. **This is not understood.** The leading hypothesis is a stale object file or an ODR
   mismatch: a header included by both translation units changed while one was not rebuilt, so the
   two disagreed about a layout. **Before concluding anything, delete `obj\MGSPWEnabler` and do a
   clean rebuild.** Putting the busy wait fix in its own module rather than `render_policy.cpp`
   avoids the known-bad path entirely, which is why the plan does that.
2. **`MGS4E_LAB_SWITCH` silently constant-folds.** If the setting is left as a lab switch, Release
   builds will compile, run, log nothing and do nothing. Verify by grepping the Release binary for
   the string `PW busy wait:` the way `lab-build-configuration` describes.
3. **Hooking `PeekMessageA` in user32 is process-wide.** RTSS, the Steam overlay and MGSPatriotFix
   pump messages too, and they will all go through our hook. The level gate is the only thing
   deciding who waits, and the added cost is a 1 ms wait that only fires when the queue was already
   empty. MGSHDFix ships exactly this for `PeekMessageW`, so it is proven in the sibling games, but
   it is the most likely source of a compatibility report.
4. **The 2 ms handoff timeout is load-bearing for shutdown.** The render thread's loop exits on the
   quit flag at display`+0x3bc8`, which it only re-reads after the wait returns. The timeout is what
   guarantees it wakes if the final frame never comes. **Do not raise it without thinking about
   exit**, and never remove it.
5. **Signature checks must fail soft.** They already do: a mismatch logs a warning and installs
   nothing. Keep that, and keep reading the bytes rather than assuming encodings. This session the
   check failed once because `xor ecx, ecx` at `+17CCF` is **`33 C9`**, not `31 C9`.
6. **Never redirect execution from these hooks.** Neither hook changes `rip`, so nothing depends on
   safetyhook's redirect semantics. The render thread's `Sleep(0)` is left to run as a cheap yield,
   and the ticker's own sleep is neutralised by setting its elapsed register equal to its period
   register (that register is recomputed from the clock at the top of every iteration, so clobbering
   it is safe). Keep it that way.
7. **The state object cache has the same module problem plus the crash mystery.** It is optional, so
   promote the busy wait fix first and ship it; the cache can follow once (1) is understood.

### The testing still owed

None of this has been soaked. Needed, with the governor at the game's default and the cache off so
the fix is judged alone:

- Menus, Codec calls, cutscenes and movies (paced to 30 by an explicit wait count at `+78070`,
  so confirm the ticker level does not disturb them), Mother Base, a mission end, and a long
  session.
- Windowed, borderless and exclusive fullscreen, and alt-tab in each.
- The ticker level (2) judged **in gameplay**. Every measurement of it so far was at a menu, where
  its contribution is inside the noise.
- A decision on the shipping default. MGSHDFix defaults its equivalent to Full; 3 is the likely
  answer here once the soak is clean.

### Lab controls

`Busy Wait Fix` in the settings: 0 off, 1 render thread, 2 also ticker, 3 also message pump. Every
hook is installed at start-up and reads an atomic level, so the live command `busywait N` switches
behaviour without a relaunch and both behaviours can be compared in one spot in one session. The
per-5-second line is `PW busy wait: level N; handoff waits ..., ticker sleeps ..., pump waits ...`.

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

## 2026-09-14: the 60 Hz stutter (root cause found, fix not built)

Full write-up with every measurement, every dead end and the instrumentation: [frame-pacing.md](frame-pacing.md).

**The cause.** The game asks the Direct3D device to create a sampler or depth-stencil state about
350 times a frame, and only ever asks for 18 distinct ones. Each call takes the device-wide lock,
which the render thread holds while it sits inside `Present` doing the driver's vsync wait. The
game thread loses 7.3 ms of every frame queued behind them, so a frame of 2.7 ms real work takes
9.6 ms of wall clock, and when that crosses the 16.7 ms tick the game's own frame-skip governor
(`+78462`) doubles a frame. Found by sampling both threads with a real stack unwind, then timing
the two calls the profiler named.

**Pick up here:**
1. Build the state object cache (section 5 of the write-up), incrementally, testing after each
   step. The first attempt crashed at start-up and the fault was never isolated: it persisted with
   the cache disabled, so do not assume object sharing is the problem until that is shown.
   Expect the per-tick Direct3D time to fall from 7.3 ms to near zero.
2. Measure again from the same lab file and the same spot: work per tick should fall from 9.6
   toward the 5 ms Fast Sync achieves, with no missed ticks.
3. Promote `Frame Skip Governor` into the Release build as a shipped setting. It is not the cause
   but it halves the damage, and it is measured and understood.
4. Fallback if the cache cannot be made safe: NVIDIA Fast Sync, user-confirmed.
5. The waitable swap chain was built and measured as no help; the render thread's `Sleep(0)` spin
   burns 47 percent of a core but is not implicated.

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
