# Peace Walker: the 60 Hz stutter (frame pacing investigation, 2026-09-14)

Status: root cause of the dominant stutter found and fixed in the Lab build (the game's
frame-skip governor); a smaller residual remains and is characterised but not yet explained.
Everything below was measured on the user's machine with the Lab build's pacing instrumentation
(all of it in `pw/src/features/draw_census.cpp`, keys in `shared/pw/settings_keys.hpp`).

## The symptom

A frame-time spike of one doubled frame (a 33 ms frame at 60 fps), about once a second, in
gameplay. The user's observations, all confirmed in the logs:

| condition | stutter |
| --- | --- |
| 60 Hz desktop, Borderless (window covering the monitor) | yes |
| 60 Hz desktop, exclusive Fullscreen | yes |
| 60 Hz desktop, Windowed (title bar visible, window filling the screen) | no |
| 60 Hz desktop, Borderless with another window in front, or the game unfocused | no |
| 120 Hz desktop, any mode | no (the earlier "120 Hz fullscreen" stutter was a different thing, below) |
| Afevis's MGSPWResolutionUnlocked instead of our ASI, 4K internal | yes |
| Afevis's plugin at 1440p internal | yes, less often, lower amplitude |
| stock game, nothing installed | yes, spaced out |
| NVIDIA Fast Sync + RTSS 60 fps limit | no |
| our ASI as of the 2026-09-13 evening commit (9dce3a1) | yes |

So: not our code (stock does it), not GPU load (GPU idle, 1440p and stock still do it, only
less), not the render scale, not the display's exact rate (59.997 Hz measured by
QueryDisplayConfig; the game's ticker is 60.000 Hz by QPC, a beat of one frame per five minutes).
The dividing line is **composed versus direct presentation**: a flip-model swap chain in a window
that exactly covers the monitor (Borderless) or in exclusive Fullscreen is scanned out directly
and `Present` returns at the vblank; a titled, covered or unfocused window is composed by DWM and
`Present` returns at once. The three smooth cases are the composed path.

## The game's frame machinery (all RVAs against the clean dump, `C:\mgspf_tools\pw\pw_text.bin`)

- **Ticker thread** `+75FD0` (created at `+775B9`, callback installed at `+77F00` -> `+78A10`):
  QPC-based, phase-carried, exactly 60.000 Hz: `loop { elapsed; while 1/60 > elapsed { timeBeginPeriod(1); Sleep((1/60 - elapsed) * 1000); timeEndPeriod(1) } tick; start += freq/60 }`.
  The tick callback `+78A10` increments the **vblank count** at `+1084498` and sets a manual-reset
  event (handle at `+1083E90`, created at `+77F00` through `+14A70`).
- **The vblank wait** (`sceDisplayWaitVblankStart` emulation): three loops wait on that event
  through the wrapper `+14AD0` (`bool WaitEvent(HANDLE*, DWORD ms /* 0 = INFINITE */)`) and
  `ResetEvent` after waking: `+78289`/`+78369` (the frame end, below) and `+78B69`. The value at
  `+1084498` after a wait is the vblanks since the game thread last consumed one: 1 on a normal
  tick, 2 when a tick was missed.
- **Frame end / frame-skip governor** `+78250..+78480`, on the game thread (tid changes per run;
  "the vblank-waiting thread" in the logs): wait for a vblank (loop 1, `+78289`); measure the frame
  in wall clock (QPC at `+782AF`/`+782BA`, converted to vblanks with a fractional threshold at
  `+783F0`); wait until the vblank count reaches the **wait count** at `+1084484` (loop 2,
  `+78369`); then adapt the wait count: a frame that measured longer than the current count
  **raises** it (`+78462`, `mov [+1084484], edi`, capped at 3), a frame comfortably shorter lowers
  it after a hold (`+7843C`). The wait count is also set explicitly at `+78070` (the 30 fps movies
  and menus; the log shows 150 missed ticks per 5 s there by design).
- **Render thread** `+17BD0`: spins on `Sleep(0)` (~170,000 calls a frame) for the handoff flag at
  display`+0x32c0`, releases 32 per-frame resources, `Present(1, 0)` (`+17CAF`), clears the flag.
  Holds the display critical section (`+0x3300`) across Present when display`+0x32f8` is set.
- **Handoff** `+175B0` (game thread): if the render thread still holds the previous frame
  (`+175DF`, flag set) the function returns and the frame is dropped; else it sets the flag. In
  practice the flag was never found set in gameplay (Frame Handoff Wait measured zero waits).
- **Swap chain**: DXGI flip model already, `FLIP_DISCARD`, 2 buffers, R8G8B8A8, `Present(1)`.
  Windowed chains ignore the refresh-rate field; in exclusive Fullscreen our policy replaces the
  game's 60/1 with the display's current rate (the separate 120 Hz fix, 2026-09-13).

## What the instrumentation showed, in order

1. **Present blocks in direct modes, not in composed ones.** Present ~8 ms a frame focused
   (vsync wait), 0.1-0.4 ms unfocused or covered. 60 presents a second in both, so no frame is
   lost overall; in the stutter case one frame is a tick late and the next comes right behind
   it (the doubled frame followed by a catch-up).
2. **Sync interval 0 changes nothing** in Borderless (the flip chain still waits for the
   compositor's slot); a third buffer changes nothing; ALLOW_TEARING Present changes nothing the
   user could see; a vblank-locked ticker changes nothing. All of those were judged with the
   heavy first-generation Sleep-hook instrumentation running (~170k hooked calls a frame with
   two clock reads and a lock each), which itself perturbed the frame-time picture. **Their
   verdicts should be considered unreliable and retested if ever needed.**
3. **The hitch sampler** (suspend and read every other thread when the render thread has spun
   20 ms for a frame): at every hitch, *every* thread was in a kernel wait. The game thread was
   in a vblank wait that had just begun; the ticker had ticked ~4 ms earlier. Nobody was late;
   the game thread had consumed a tick without producing a frame.
4. **Lost-wakeup theory** (two threads reset one manual-reset event): a 1 ms timeout on the
   vblank wait (`VBlank Wait Timeout`) changed nothing. Not the cause.
5. **Handoff-drop theory**: `Frame Handoff Wait` measured zero occurrences. Not the cause.
6. **The vblank wait log** (per wait: ticks consumed, work since the previous wait, time in
   Sleep/event waits, critical sections and D3D calls in that work): every governor-induced
   missed tick was **a second vblank wait with 0.00 ms of work in between**, i.e. the frame end's
   loop 2 waiting for the raised count. The game thread's per-frame work averages 9.5-10 ms at
   16x with a 16.7 ms budget.
7. **Governor off** (`Frame Skip Governor = false`: the store at `+78462` skipped, explicit
   count and lowering intact): the log counts 2-4 prevented raises a second, exactly the old
   stutter rate; the user: "significantly better".

Why direct presentation trips the governor: with Present returning at the vblank, the render
thread's cycle and therefore the game thread's frame end land right on the vblank boundary, and
the wall-clock frame measurement comes out a fraction over 16.67 ms often enough (2-4 times a
second) to raise the count. Under composition frame ends land mid-period and never measure long.
Higher internal resolution nudges the frame end later, so Afevis's 4K and our 16x trip it more
than 1440p, and stock least of all. At 120 Hz the frame end lands on 8.3 ms boundaries and a raise
costs half as much.

## The residual (open)

With the governor off the game thread still misses ~1.3 ticks a second (6-7 per 5 s) and the
display gets 58-59 presents a second. These misses are different: the work between waits runs to
**16.8-17.2 ms**, i.e. the game thread itself is just over budget, then recovers. Where those
milliseconds go is unknown; the per-tick detail (F10 in the Lab: Sleep/event waits, critical
sections, D3D calls with the longest single one) was built but never captured for these misses
(the run ended first). Candidates, in order: a D3D call on the game thread waiting for the GPU
(Map of a busy dynamic buffer, FinishCommandList), the display critical section (measured 0.00
ms so far), plain CPU work at 16x. The user's note that the GPU clock quadruples when the game is
unfocused (Present no longer blocking, GPU never idles) suggests the GPU runs at idle clocks while
focused; a frame rendered at idle clocks may also run long. Untested: NVIDIA "Prefer maximum
performance" for the exe.

## The Lab knobs and triggers (all `[Graphics]`, Lab build only unless noted)

| key | meaning |
| --- | --- |
| `Fullscreen Refresh Rate Fix` (both builds) | on: exclusive Fullscreen asks for the display's current refresh rate (the 2026-09-13 120 Hz fix); off: the game's 60/1 stands |
| `Fullscreen Resolution Fix` (both builds) | on: exclusive Fullscreen runs at the selected Screen Resolution (the `+1A25B` hook and the buffer guard); off: the monitor's largest mode |
| `Frame Skip Governor` | **the fix**: false skips the raise at `+78462` |
| `VBlank Wait Log` | per-tick log on the vblank waits; `PW vblank:` 5 s summaries; F10 / live `skips N` arms N per-miss detail lines |
| `Hitch Sampler` | light Sleep import hook; F9 / live `sample N` arms N thread samples on the next hitches (`PW sampler:`) |
| `Pacing Log` | the heavy first-generation instrumentation (import hooks on every Sleep and wait, tick counter, 5 s `PW pacing:` summaries): perturbs timing, off for judging runs |
| `Frame Pacing` | Game / Display (ticker period = 1/refresh) / VBlank (ticker waits on `IDXGIOutput::WaitForVBlank`) |
| `Present Sync` | -1 the game's; 0..4 forced |
| `Swap Chain Buffers` | 0 the game's (2); else the flip chain's count |
| `Allow Tearing` | chain created with `ALLOW_TEARING`, windowed Presents `(0, ALLOW_TEARING)` |
| `VBlank Wait Timeout` | ms; a timeout on the vblank event wait |
| `Frame Handoff Wait` | ms the game thread waits for the render thread instead of dropping a frame |

Triggers in the game: **F11** census, **F10** twenty missed-tick detail lines, **F9** six thread
samples. Live file `logs\MGSPWEnabler_live.txt`: `census N`, `skips N`, `sample N`.

## Testing rules learned tonight

- `boot.ps1 -LabConfig` reads `MGSPWEnabler.lab.settings`; regenerate it from the user's
  `MGSPWEnabler.settings` before a launch and change one key. A stale lab file cost a run.
- Check in with the user before launching and before closing while they are live testing.
- `tasklist` truncates the game's image name: match on `METAL GEAR`, not `PEACE WALKER`.
- Judge by the user's frame-time graph; my counters ran alongside instrumentation that itself
  perturbed timing until it was gated (`Pacing Log`).
- Use Display on a 16:9 desktop builds a 484x272 canvas (decision deferred); test with "16:9".
