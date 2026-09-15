# Peace Walker: the 60 Hz stutter (frame pacing, 2026-09-14)

**Root cause, established by measurement:** the game asks the Direct3D 11 **device** to create a
sampler or depth-stencil state about **350 times a frame**, and in a whole session it only ever
asks for **18 distinct ones**. Every one of those calls takes the device-wide lock. The render
thread holds that lock while it sits inside `Present`, where the display driver performs the vsync
wait as a sleep loop for about eight milliseconds. So the game thread loses about **7.3 ms of
every frame** queued behind those creations, its frame takes 9.6 ms of wall clock to do 2.7 ms of
work, and when that crosses the 16.7 ms tick the game's own frame-skip governor doubles a frame.
That doubled frame is the stutter.

**Largely fixed, 2026-09-15.** A state object cache (Lab: `State Object Cache`) removes the state
creations entirely, and with them the Direct3D stall. The game thread's lock wait vanished from the
profiler, its work per tick fell, and the user reports the stutter gone bar occasional small jumps.
What remains is one layer down and is the true root cause: **the render thread holds the game's own
display critical section across `Present`**, entering it at `+17C1D`, presenting at `+17CAF` and
leaving at `+17CC9`. The game thread then queues on that section for about six milliseconds a
frame. Releasing it around the present is the remaining work; see section 5.2.

Everything below was measured on the user's machine with the Lab build. The instrumentation is in
`pw/src/features/draw_census.cpp`, the keys in `shared/pw/settings_keys.hpp`.

## 1. The symptom

A frame-time rise at a set interval, plainly visible on a frame-time graph, in gameplay. From a
twenty minute run on the user's own settings (16:9, render scale 16, Borderless):

- The interval is strikingly regular: a **median of 50 frames** between the start of one hitch and
  the next, about 0.83 s at 60 fps, with the commonest gaps between 48 and 54.
- Most hitches are one frame, but they latch: a dozen runs of 7 to 16 consecutive frames, a
  quarter of a second at half rate.
- With the governor at the game's default a hitch frame measures 33 ms (140 of 400 land exactly
  there). With the governor switched off the same beat shows as roughly 20 ms instead, which the
  user describes as micro-stutter: "it still feels terrible, but it's not as obvious".

Conditions, all user-confirmed:

| Condition | Stutter |
| --- | --- |
| 60 Hz desktop, Borderless covering the monitor | yes |
| 60 Hz desktop, exclusive Fullscreen | yes |
| 60 Hz desktop, Windowed with a title bar | no |
| 60 Hz desktop, another window in front (game still rendering and taking input) | no |
| 120 Hz desktop | no |
| NVIDIA Fast Sync | no |
| NVIDIA Fast Sync plus an RTSS 60 fps cap | no, and perfectly flat |
| GPU clocks pinned at 2550 MHz via maximum performance | unchanged, still stutters |
| Afevis's MGSPWResolutionUnlocked at 4K internal, our ASI off | yes |
| Afevis's plugin at 1440p internal | yes, less often, lower amplitude |
| stock game, nothing installed | yes, more spaced out |
| our ASI as of the 2026-09-13 evening commit | yes |
| process affinity restricted to a core pair | unchanged |
| process affinity restricted to one core | present but irregular, skipping beats |

So: not our code, not GPU load, not GPU clocks, not the render scale, and not the display's exact
rate (59.997 Hz measured against the game's 60.000 Hz ticker, a beat of one frame per five
minutes, nothing like 0.83 s).

## 2. The game's frame machinery

RVAs against the clean dump, `C:\mgspf_tools\pw\pw_text.bin`.

- **Ticker thread** `+75FD0`, created at `+775B9`, callback `+78A10`. QPC based, phase carried,
  exactly 60.000 Hz. Its wait loop at `+76020` raises the timer resolution to 1 ms, sleeps the
  whole remaining milliseconds, drops the resolution, and re-reads the clock, so the last
  sub-millisecond of each tick degenerates into `Sleep(0)`. The callback increments the vblank
  count at `+1084498` and sets a manual-reset event whose handle is at `+1083E90`.
- **The vblank wait**, the emulation of `sceDisplayWaitVblankStart`: three loops wait on that
  event through the wrapper `+14AD0`, at `+78289`, `+78369` and `+78B69`, resetting it after each
  wake.
- **Frame end and frame-skip governor** `+78250..+78480`, on the game thread. Waits for a vblank,
  measures the frame in wall clock, waits until the vblank count reaches the wait count at
  `+1084484`, then adapts that count: a frame measuring longer than the current count **raises**
  it at `+78462` (capped at 3), a comfortably shorter one lowers it at `+7843C`. The count is also
  set explicitly at `+78070`, which is how the 30 fps menus and movies are paced.
- **Render thread** `+17BD0`: spins on `Sleep(0)` at `+17CD7` waiting for the handoff flag at
  display`+0x32c0`, releases the frame's resources, calls `Present` at `+17CAF`, clears the flag.
- **Handoff** `+175B0`, on the game thread: if the render thread still holds the previous frame it
  returns and the frame is dropped. Measured at zero occurrences in gameplay.
- **Message pump**, main thread, loop at `+77610`: `PeekMessageA`, and when nothing is queued,
  `Sleep(1)`, repeat. The game imports no `MsgWaitForMultipleObjects`, no `WaitMessage`, no
  `GetMessage` and no waitable timers.
- **Swap chain**: DXGI flip model already, `FLIP_DISCARD`, two buffers, `Present(1, 0)`.

## 3. The measurements that found it

### 3.1 The game thread is not working, it is waiting

Per tick, in the stuttering configuration, with every category timed:

| | Stuttering | Fast Sync |
| --- | --- | --- |
| Wall clock per tick | 9.6 to 9.8 ms, max 17.0 to 17.3 | 3.4 to 5.2 ms, max 6.2 |
| Cycles actually executed | 2.7 ms | not separately captured |
| Not executing | 7.0 ms | |
| Direct3D calls, all 8933 including draws and unmaps | 0.27 ms | |
| Sleeps and event waits | 0.01 ms | |
| `Sleep(0)` yields | 0.00 ms, zero calls | |
| Critical sections | 0.00 ms | |
| Missed ticks per 5 s | 6, in every window | 0 in nine of ten windows |

Both runs were the same spot in the same session with one variable changed, after an earlier
cross-session comparison left room for scene differences. The executed figure comes from
`QueryThreadCycleTime` against the processor's nominal 4192 MHz, and agrees with the
`GetThreadTimes` reading, so neither is an accounting artifact.

### 3.2 Where the game thread actually sits

Sampling profiler, 1360 samples over three seconds, in mission:

| | |
| --- | --- |
| `NtWaitForSingleObject` | 43.8% |
| `NtWaitForAlertByThreadId` | 39.4% |
| everything else, all game code included | under 3% |

The first is healthy: it is the tick wait. The second is the answer, and its call chain is:

```
NtWaitForAlertByThreadId < ntdll lock code < d3d11.dll
  < GAME+185E2 / +18538 < GAME+187E7 < GAME+5727D
```

The game thread blocks inside the Direct3D 11 runtime, on the runtime's own lock, in the draw
submission and vertex upload path.

### 3.3 Who holds the lock

Sampling profiler on the render thread, same moment:

| Where the render thread is | Share |
| --- | --- |
| `Sleep(0)` handoff spin at `GAME+17CD7` | 47% |
| Blocked inside `Present` | 42% |
| Everything else | 11% |

```
GAME+17CB2 (Present) < RTSS hook < our hook < Steam overlay
  < dxgi.dll < d3d11.dll < nvwgf2umx.dll < SleepEx < NtDelayExecution
```

The driver implements the vsync wait as a sleep loop inside `Present`, and the device lock is held
for its duration. That is the seven milliseconds the game thread loses.

### 3.3a The calls that block, and what they cost

Disassembling the game frames the profiler named identifies both blocking calls as **device**
vtable entries, not context ones:

| Site | Vtable offset | Method |
| --- | --- | --- |
| `GAME+18532` | 0xA8, slot 21 | `CreateDepthStencilState` |
| `GAME+185DC` | 0xB8, slot 23 | `CreateSamplerState` |

They sit in a sixteen-entry loop at `GAME+18500` with `OMSetDepthStencilState` (context slot 36)
alongside, so the game recreates its state objects whenever its own state cache goes dirty.
Timing those two calls and nothing else changed the whole picture:

| | At the title | In gameplay |
| --- | --- | --- |
| device state creations | 11 a frame | **350 a frame** |
| their cost | 0.01 ms a frame | **7.9 ms a frame** |
| distinct descriptions, whole session | 12 | 18 |

The worst tick then reads `17.19 ms, of it 3.53 executed, 13.97 in 9461 D3D call(s) (longest
CreateSamplerState)`, and the per-tick Direct3D mean rises from 0.27 ms to 7.29 ms, which is
exactly the time that had been unattributed. The calls are not expensive in themselves, as the
title shows: the cost is entirely contention for the device lock.

This is also why nothing else worked. Buffer counts, the waitable object, the tearing flag, the
sync interval and the ticker all change **when** Present blocks, not **that** it blocks, so the
lock is held either way. Fast Sync works because it stops Present blocking at all.

### 3.4 Thread CPU

Per five seconds in gameplay, as a percentage of one core:

| Thread | Share | Priority |
| --- | --- | --- |
| render | 65 to 71% | 0 |
| game | 14 to 21% | 0 |
| whole process, 63 threads | 88 to 98% | |

Both threads are at normal priority, so the render thread's spin is not starving anything by rank.
Its share is the `Sleep(0)` spin, which issues a syscall per call at roughly 170,000 calls a frame.

## 4. What was ruled out, and how

| Theory | Verdict |
| --- | --- |
| Our own code | Stock game and Afevis's plugin both stutter |
| GPU load or GPU headroom | Persists at 1440p internal and on the stock game |
| GPU clock ramp | Persists with clocks pinned at 2550 MHz |
| Display refresh beating against the ticker | The beat would be one frame per five minutes, not 0.83 s |
| The frame-skip governor as the cause | Removing it keeps the same beat at lower amplitude, so it is an amplifier |
| A lost wakeup on the shared vblank event | A 1 ms timeout on that wait changed nothing |
| The frame handoff dropping frames | Measured at zero occurrences |
| A periodic task inside the game | Composed presentation is perfectly flat; a timer would still fire |
| The MGS2 and MGS3 busy-wait regression | A different shape; see section 7 |
| Lock contention in the game's own code | Critical sections measured 0.00 ms throughout |
| The game thread yielding | Zero `Sleep(0)` calls on it |
| Slim locks or condition variables in game code | Zero calls to either condition variable form or `WaitOnAddress` |
| Involuntary preemption | Both threads at normal priority; the profiler shows a wait, not a ready state |
| Swap chain buffer count | Three buffers changed nothing |
| `DXGI_PRESENT_DO_NOT_WAIT` | Zero retries: the flag covers a previously queued frame, not the vsync wait |

Judged by eye under the first-generation instrumentation and therefore **unreliable, to be
retested if they become relevant**: sync interval 0, tearing-allowed Present, a third buffer, and
a vblank-locked ticker. That instrumentation hooked every one of the render thread's 170,000
`Sleep(0)` calls a frame with two clock reads and a lock, and perturbed the thing it measured.

## 5. The fix

### 5.1 The state object cache (built and measured, 2026-09-15)

The game recreates **all four** Direct3D state object types every frame and only ever asks for a
few dozen distinct ones:

| Site | Device vtable | Method |
| --- | --- | --- |
| `GAME+184AB` | 0xA0, slot 20 | `CreateBlendState` |
| `GAME+18532` | 0xA8, slot 21 | `CreateDepthStencilState` |
| `GAME+18411` | 0xB0, slot 22 | `CreateRasterizerState` |
| `GAME+185DC` | 0xB8, slot 23 | `CreateSamplerState` |

The cache keys each description field by field, never on the raw struct bytes, because the
descriptions sit on the game's stack and carry whatever padding was there before. It hands back the
object the game asked for last time with a reference added, and keeps one of its own for the life
of the process. 42 objects covered a whole session.

Measured, same spot, same session:

| | Before | After |
| --- | --- | --- |
| Device state creations | 350 a frame | 0 |
| Direct3D time per tick | 7.29 ms | 0.18 ms |
| Work per tick, mean | 9.6 ms | 8.7 ms |
| Work per tick, worst | 17.0 to 17.5 ms | 16.7 to 16.9 ms |
| Missed ticks per 5 s | 6, every window | ten seconds at a time with none, else 5 to 6 |
| Game thread CPU executed per frame | 2.70 to 2.84 ms | 2.38 to 2.58 ms |

It also saves about 0.2 ms of genuine CPU work a frame, roughly eight percent of everything the
game thread executes, which matters more on a slower processor than it does here.

**It lives in `draw_census.cpp` beside the hooks rather than in `render_policy.cpp`, deliberately.**
Implementing it in `render_policy.cpp` and calling across made the game abort at start-up every
time, even with the cache switched off, and even with the call reduced to reading a counter in a
log line that never ran before the crash. The hooks provably completed and returned `S_OK`; the
crash came after. Moving the code into the same translation unit made it go away. The cause is not
understood, and it must be before this is promoted to Release, which needs the same logic reachable
from `render_hooks.cpp`.

### 5.2 The remaining stall: the display critical section

The render loop enters the display object's critical section (display`+0x3300`) at `+17C1D`, calls
`Present` at `+17CAF` **inside it**, and leaves at `+17CC9`. So one of the game's own locks is held
across the whole vsync wait, and the game thread blocks on it when handing over the next frame:
measured at 6.17 ms mean and 14.31 ms worst, in the `locks` bucket that had read 0.00 all night
until the Direct3D stall was taken away.

The fix is to leave that section immediately before `Present` and re-enter immediately after, in
the Present hook we already own. The display object pointer is at `+15969D8`. Not built. It is a
real behavioural change, since it opens exactly the window in which the game thread starts
recording the next frame, so it needs a soak rather than a spot check.

### 5.3 What was tried and did not help

**A waitable swap chain:** create it with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`,
wait on the handle from `GetFrameLatencyWaitableObject` before presenting, and let `Present`
return straight away. The vsync wait then happens on the render thread outside any Direct3D call,
so the device lock is free and the game thread keeps recording. Vsync pacing is preserved and
nothing tears.

It was built and measured on 2026-09-14 and **did not help**: the chain was created, the frame
slot handle was acquired, the render thread did wait on it, and the numbers did not move at all
(9.6 ms mean, 17 ms worst, six outliers), because Present still blocks for want of a free buffer.
Three buffers together with the waitable object changed nothing either. Kept here because the
reasoning is sound for a different engine and because the code was written.

The work, and the risk:

- The game creates its chain through the legacy `IDXGIFactory::CreateSwapChain`, which cannot
  carry the waitable flag. Creation has to be intercepted and rebuilt through
  `CreateSwapChainForHwnd`, carrying the fullscreen description across.
- Any `ResizeBuffers` must keep the flag.
- The wait goes in the existing Present hook, before the original call.
- Swap chain creation is the one place where a mistake shows as a black screen or a broken
  fullscreen transition rather than as a number in a log.

Interim workaround that works today, user-confirmed: **NVIDIA Fast Sync**. It removes the block
from Present, and the game thread's work falls from 9.6 ms to about 5 ms with zero missed ticks.
An RTSS 60 fps cap on top makes the graph perfectly flat but changes nothing the game thread can
see, so it is stabilising presentation only.

## 6. The Lab instrumentation

All keys under `[Graphics]`, Lab build only unless noted, all off by default.

| Key | What it does |
| --- | --- |
| `Fullscreen Refresh Rate Fix` (both builds) | exclusive Fullscreen asks for the display's current refresh rate; the separate 120 Hz fix of 2026-09-13 |
| `Fullscreen Resolution Fix` (both builds) | exclusive Fullscreen runs at the selected Screen Resolution rather than the monitor's largest mode |
| `Frame Skip Governor` | false skips the raise at `+78462`: the amplifier, not the cause |
| `VBlank Wait Log` | the per-tick accounting: wall clock, executed cycles, and the split into Direct3D, waits, yields, slim locks and plain work; a five second summary per thread; also enables the per-thread CPU line and the ntdll wait hooks |
| `Hitch Sampler` | the light `Sleep` hook and the F9 thread sampler |
| `Pacing Log` | the first-generation instrumentation. Perturbs timing. Off for anything judged by eye |
| `Frame Pacing` | Game, Display or VBlank: how the 60 Hz ticker paces itself |
| `Present Sync` | -1 the game's, else a forced sync interval |
| `Swap Chain Buffers` | 0 the game's two, else the flip chain's count |
| `Allow Tearing` | tearing-capable chain and tearing-flagged presents |
| `Present Without Blocking` | Present with do-not-wait and retry. Measured a no-op here |
| `VBlank Wait Timeout` | a timeout on the game's vblank event wait |
| `Frame Handoff Wait` | milliseconds to wait for the render thread instead of dropping a frame |

In-game triggers, all needing `Live Commands`:

| Key | Command | What |
| --- | --- | --- |
| F11 | `census N` | the draw census |
| F10 | `skips N` | the next N long ticks, broken down by category |
| F9 | `sample N` | every thread's state at the next N hitches |
| F8 | `profile [N]` | sample the game thread for N seconds |
| F7 | `profile render [N]` | sample the render thread |

The profiler unwinds with `RtlLookupFunctionEntry` and `RtlVirtualUnwind`, so its chains are real
frames rather than plausible-looking stack values.

## 7. Comparison with the MGS2 and MGS3 busy wait

MGSHDFix fixes an idle-wait regression Konami introduced in patch 1.4.0: window event handling
moved to its own thread which polled continuously instead of sleeping, pegging a core at 80 to 100
percent. The fix has two halves, shipped as Half and Full. Half hooks `PeekMessageW` and, on an
empty queue, calls `MsgWaitForMultipleObjects` with a 1 ms timeout. Full adds a hook on the game's
frame wait that blocks on a high-resolution waitable timer for the remaining time less a 1 ms
margin, and holds 1 ms timer resolution for the session.

Peace Walker is a different shape. Its message pump does sleep, one millisecond per pass. Its
ticker is accurate. It does have a genuine spin, the render thread's handoff loop at 47 percent of
a core, but that is not what stalls the game thread: the stall is a lock held across a driver wait.
The lesson that does carry over is the second half of their fix, which is the same idea as the
waitable swap chain: do not block inside something that holds a resource others need.

## 8. Instrumentation that went wrong, so it is not repeated

- **Hooking `RtlAcquireSRWLockExclusive` killed the process**, because the hooking library, the
  loader and our own logging all take slim locks, so the hook is reentrant by construction. The
  three blocking entry points (`RtlSleepConditionVariableSRW`, `RtlSleepConditionVariableCS`,
  `RtlWaitOnAddress`) are safe: they are only entered when a thread actually blocks.
- **`RtlVirtualUnwind` without a structured exception handler killed the process.** It reads the
  stack without validating it, and a thread suspended part way through a prologue presents a frame
  it cannot follow. The unwinder now contains faults and a bad sample is simply shorter.
- **The first long-tick trigger fired on the wrong event.** Triggering on "consumed two vblanks"
  catches the wait that follows an expensive tick, which by definition contains no work, so three
  separate attempts printed nothing but zeroes. It now triggers on the tick's work exceeding that
  thread's own running mean, which also keeps the 30 fps menus from qualifying.
- **`GetThreadTimes` was doubted and turned out to be right.** `QueryThreadCycleTime` agreed with
  it. The cycle counter is still the better instrument, because it cannot be dismissed.

## 9. Testing rules

- `boot.ps1 -LabConfig` reads `MGSPWEnabler.lab.settings`. Regenerate it from the user's
  `MGSPWEnabler.settings` before every launch and change only the key under test. A stale lab file
  once produced a run with no supersampling that the user had to diagnose from the picture.
- Check in with the user before launching and before closing while they are testing.
- `tasklist` truncates the game's image name: match on `METAL GEAR`.
- Compare within one session and one spot in the game. Cross-session comparisons leave room for
  scene differences, which is how the first Fast Sync comparison nearly went wrong.
- The user's frame-time graph is the judge of the picture; the logs are the judge of the cause.
