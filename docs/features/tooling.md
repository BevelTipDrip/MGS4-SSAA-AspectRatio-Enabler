# Tooling

**Settings:** none user-facing. Everything here is off by default, absent from `MGS4Enabler.exe` and, outside a Lab build, from `MGS4Enabler.asi`
**Implements:** `asi/src/features/stage_automation.cpp`, `asi/src/features/render_pipeline.cpp`,
`asi/src/core/config.cpp` (`ChooseFile`), `C:\mgspf_tools\*.ps1`

Development harness. Since 2026-09-04 it is compiled only into the **Lab** configuration
(`build\build.cmd lab`, output `bin\Lab`): in a Release build every switch here is a compile-time
constant (`shared/lab.hpp`), the code and its strings are absent from the ASI, and lab mode itself
does not exist. A Lab build still does nothing unless the game is booted in **lab mode**.

## Lab mode

The research keys are only read from `MGS4Enabler.lab.settings` in the game root, and only when
the harness has dropped a `MGS4Enabler.lab` marker next to it less than fifteen minutes before
the game came up. The ASI deletes the marker as it reads it, so a lab boot cannot leak into the
user's next normal launch; a stale marker is ignored and cleared the same way. The user's own
`MGS4Enabler.settings` is never touched or swapped. The log says `LAB MODE` right after the
settings-file line when it is in effect.

## The test loop

`boot_to_aim.ps1` goes from nothing running to a captured, self-assessed screenshot of Snake
aiming in about 90 seconds, with no keyboard involved and no save file:

```
steam://rungameid  ->  Enter  ->  Enter  ->  F7 (fast-load a stage)
  ->  Enter (confirm, when the screen shows it)  ->  gameplay, AK-102 equipped
  ->  hold aim  ->  screenshot  ->  CROSSHAIR PRESENT / ABSENT
```

Exit code 0 means the reticle rendered, 2 means it did not, and 3 means **the game crashed** -
reported from its own dump rather than as a timeout. That distinction matters more than it
sounds: crashes previously surfaced as "TIMEOUT waiting for the stage registry", which reads
as a slow run, and an hour went into investigating the wrong things because of it.

Every exit path closes the game. Pass `-KeepOpen` to leave it up for inspection. This is not
just tidiness on someone's desktop: a live `mgs4.exe` keeps Steam's `Running` flag set, and
while that flag is set Steam silently ignores the next `steam://rungameid`. The close is a
`WM_CLOSE` with a 15s grace period, falling back to a kill only for a hung process — a forced
kill is itself one way that flag gets stuck.

### Things that took a run each to learn

- **Launch through Steam, never the wrapper directly.** Running `Launcher\launcher.exe` denies
  `mgs4.exe` Steam's environment, so its DRM stub asks Steam to relaunch and exits 53. Steam then
  shows a custom-arguments prompt drawn *inside* its own client window — not a top-level window,
  so it cannot be found by enumeration and Enter does not reach its Continue button.
- **Kill `launcher_original.exe` too.** With a launcher-bypass mod installed, its bypass falls
  back to that executable when `CreateProcessW` fails, but the fallback can never work: Unity
  derives its data folder from the executable name. It puts up a modal instead, and that modal
  keeps Steam's `Running` flag set, which makes every later `steam://rungameid` silently do
  nothing.
- **The log is truncated per session.** A byte offset taken before a run points past the end of
  the new log, and every wait times out having read nothing.
- **Gate on the HUD, not a timer or the engine's transition flag.** The confirmation prompt
  appears a variable few seconds after the request, and pressing Enter early navigates whatever
  menu is still up. `StageRequestBlocked` is no help: the per-frame hook does not tick during a
  load, so it never observes either edge.
- **Screen capture and `SetForegroundWindow` fail while the display is asleep**
  (`CopyFromScreen` throws "the handle is invalid"). The game keeps running and logging, so it
  presents as the automation breaking for no reason. `keep_display_awake.ps1` holds
  `ES_DISPLAY_REQUIRED` to prevent it.
- **The game will not launch without a real display output, and this is not the same problem.**
  Capture surviving a powered-off panel says nothing about the game: `mgs4.exe` reaches about
  85MB, faults with `EXCEPTION_ACCESS_VIOLATION` and writes a crash dump, every time. Five
  crashes in one session came from this. **A headless run needs a DisplayPort dummy plug or a
  virtual display driver** - keep-awake only convinces Windows a display object exists, which
  is enough to composite a desktop and not enough to present a swapchain.
- **Give the display time to settle after switching a panel back on.** One launch crashed
  immediately after the monitor was powered up, with the topology still churning:
  `\.\DISPLAY17` as primary, four monitor entries of which two were stale, and a Generic
  Monitor rather than the real panel reporting as primary. Later launches on the same machine
  and build were fine.

## Stage automation

Resolves the engine's fast-load machinery by signature and hooks its per-frame tick. The stage
registry holds **233 stages**, populated about 1.5s after init — before the title screen. Names
are `sNNaMMl[_variant]`: act `NN`, area `MM`, `_D` marking a cutscene variant.

`s01a10l_1` (id `0xEF`) loads with an AK-102 equipped, which is why it is the default test stage.
`GetAsyncKeyState` is system-wide, so the hotkey can be driven by a script **without focusing the
game** — which matters, because alt-tabbing pauses the game.

## Screen detectors

Both were validated against known screenshots before being trusted:

| Script | Question | Separation |
| --- | --- | --- |
| `in_game.ps1` | is this playable gameplay? | menus and prompts score **0**, gameplay **382-540** |
| `crosshair.ps1` | is the reticle drawn? | present **32-39**, absent **exactly 0** |

`edge_energy.ps1` and `edge_energy2.ps1` measure edge sharpness, intended for detecting whether
FXAA is active. **Neither is trustworthy as an automated gate** — see the limits section in
[anti-aliasing.md](anti-aliasing.md), where both failed to separate FXAA on from off on captures
whose difference is obvious to the eye at native resolution. They are kept because the percentile
version is a better starting point than the mean, not because either currently works.

`shot.ps1` captures the game's client area rather than the desktop, so the detectors' fractional
regions stay valid at any window size.

**Captures are not native resolution.** On this machine every capture comes out 1280x720: the
capturing process is not DPI-aware, so at 150% display scaling a fullscreen 4K client area reads
as 2560x1440, and `shot.ps1` then halves it (`-Divisor 2` default). Fine for presence detection
and proportion judgments; useless for pixel-exact measurement or AA comparison, which the user
does against the real screen. Pass `-Divisor 1` for the un-halved virtualized size — full native
capture would additionally need the process made DPI-aware.

## UI research diagnostics

Behind `[Graphics]` keys in `MGS4Enabler.lab.settings` (lab mode only; see above). The useful ones:

| Key | What it does |
| --- | --- |
| `Log Viewports` | per-draw logging: shader, call site, vertex decode, layouts, anchors |
| `Log UI Layout` | hooks the UI layout converter (`+439810`), logs every rect and dumps the mapping written into each layout object. Cheap, unlike the two above. F6 re-arms the log budget |
| `Watch Staging Writes` | aim-gated sampling, staging traces, hardware write watches |
| `Scan Narrowing Conversions` | read-only sweep for 16-bit narrowings of converted coordinates |
| `Disassemble RVA` / `Bytes` | in-process disassembly, since `.text` is encrypted on disk |

**`Log Viewports` and `Watch Staging Writes` are extremely expensive.** They hook every draw
call and add per-draw memory resolution; with both on the game is unplayable regardless of GPU.
They were left on in a settings file once and the resulting stutter looked like a rendering
regression.

**They are also not independent.** `Watch Staging Writes` alone samples nothing: its aim-gated
draw sampling lives inside the command-list hooks that only `Log Viewports` installs, and at
100% internal scale with no window override the D3D12 device hook is skipped entirely. To get
aim-time draw samples you need `Log Viewports=1`, `Watch Staging Writes=1`, **and** either a
scale above 100 or a window-resolution override. Two silent runs went into learning this.

The `ANCHOR` per-draw `v8` sampler was broken for a while in exactly this silent way — hooks
installed, 24000 draw samples, zero ANCHOR lines — because the pipeline-creation analysis that
populated its anchor map had been removed along with the old shader workaround, leaving the
consumer checking a flag nothing set. It now detects the six-float4 per-draw stream from each
pipeline's input layout (any consecutive run, any slot), and logs the first few *implausible*
reads instead of dropping them, so a wrong offset is diagnosable from the log rather than
indistinguishable from "no anchors".

## Error popups seen during automation

Both of these block a run and neither comes from the game, so neither appears in its log. Screenshot:
[`TL-popups-steam-args-and-launcher-original.jpg`](evidence/TL-popups-steam-args-and-launcher-original.jpg).

### Steam - "Launch Game with custom arguments"

> **Launch Game with custom arguments**
> METAL GEAR SOLID 4: Guns of the Patriots - Master Collection Version is attempting to launch
> with optional parameters shown below:
> 
> eu -lan en -selfregion EU -resolution 0 -launcherpath launcher.exe -ctrltype PS5
> -launcherroot "F:\SteamLibrary\steamapps\common\METAL GEAR SOLID 4\Launcher"
> 
> If you did not request this launch or do not understand these options, select Cancel.

- **Owner:** `steamwebhelper`, drawn **inside** the main Steam client window
- **When:** running `Launcher\launcher.exe` directly, which denies `mgs4.exe` Steam's
  environment. Its DRM stub then asks Steam to relaunch and exits with 53
- **Why it is awkward:** it is not a top-level window, so enumerating windows never finds it and
  Enter does not reach its Continue button. It blocks indefinitely
- **Avoid it:** launch with `steam://rungameid/2492670`. Steam starts the wrapper itself with its
  environment intact, the DRM stub is satisfied first time, and no prompt appears

### launcher_original.exe - "Data folder not found"

> **Error**
> Data folder not found
> 
> Details:
> Application folder: F:\SteamLibrary\steamapps\common\METAL GEAR SOLID 4\Launcher
> There should be a 'launcher_original_Data' folder next to the executable

- **Owner:** `launcher_original.exe`, a normal top-level window
- **When:** a launcher-bypass mod (not part of this one) falls back to the renamed
  original after `CreateProcessW` on the game fails
- **Why it matters:** the fallback can never succeed, because Unity derives its data folder from
  the executable name and the folder is still `launcher_Data`. Worse, the modal keeps Steam's
  `Running` flag set, and while that flag is set Steam **silently ignores** every later
  `steam://rungameid` - which presents as launching doing nothing at all
- **Avoid it:** kill `launcher_original.exe` alongside `mgs4.exe` and `launcher.exe` during
  teardown

---

## Active bugs

None recorded.

---

## Fixed

None recorded.

---

## Retracted / unverified

None recorded.
