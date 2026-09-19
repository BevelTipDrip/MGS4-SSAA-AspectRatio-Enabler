# Peace Walker: the 60 Hz stutter (frame pacing, 2026-09-14)

> **2026-09-15, superseded in part.** The cause of the *visible* stutter is section 5.6: the game
> never blocks, and its three spin loops cost about a full core. The device-lock analysis below is
> correct and its fix (the state object cache, 5.1) is worth keeping for its own sake, but removing
> that stall alone did not fix the picture. Read 5.6 first.

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

### 5.4 Releasing the display lock before the flip (tried, abandoned 2026-09-15)

The render loop holds the game's display critical section across `Present`, so the game thread
blocks on it for about 6 ms a tick (3.3a). Releasing it **across** `Present` crashes inside
`nvwgf2umx`: the lock is load-bearing. Waiting for the vertical blank ourselves with the lock
released fixed the game thread completely but wrecked the cadence, because a fixed wait does not
self-correct. The third attempt kept `Present` as the pacing authority and gave the lock up only
for the idle stretch before the flip.

**First anchor, wrong.** The sleep target was computed from when the previous `Present` returned,
which is a time we had ourselves moved by sleeping. The error compounded instead of correcting:

| Frame | Interval | Time inside `Present` |
| --- | --- | --- |
| n | 26.0 ms | 7.3 ms |
| n+1 | 27.7 ms | 5.6 ms |
| n+2 | 29.2 ms | 4.1 ms |
| n+3 | 30.8 ms | 2.7 ms |
| n+4 | 33.4 ms | 0.1 ms |

About 1.5 ms of drift a frame until `Present` missed the flip entirely, after which the frame
locked at 33 ms for a second or more. Three such runs in one session.

**Second anchor, correct but not worth it.** `IDXGISwapChain::GetFrameStatistics` reports the
wall-clock time of a real vertical blank and the refresh count with it, so the target comes from the
display's own clock and an overshoot is corrected on the next frame. With that, a margin sweep
against doubled ticks:

| Margin | Lock released a frame | Doubled ticks / 5 s | Blocked on locks | Worst tick |
| --- | --- | --- | --- | --- |
| 1 ms | 5.05 ms | 0 | 0.07 ms | 15.8 ms |
| 2 ms | 4.20 ms | 0 | 0.00 ms | 3.9 ms |
| 3 ms | 3.30 ms | 0 | 0.06 ms | 13.5 ms |
| 5 ms | 1.46 ms | 6 | 6.62 ms | 16.9 ms |
| 7 ms | varies | 4 | 4.27 ms | 25.5 ms |

By the numbers the lock contention goes to zero at 2 ms. **The user could not see any improvement,
and judged it slightly worse.** That verdict is confounded: with the yield enabled,
`GetFrameStatistics` ran on **every present**, inside the present path, taking the same device lock
this whole investigation is about. The yield was paying for its own phase reading at the worst
possible place.

**Conclusion: the game thread's wait on the display lock is real but is not what the user sees.**
Removing it entirely changed nothing visible. Before this is retried, the phase must be sampled
once every 30-60 presents and extrapolated in between, since flip times are arithmetic once the
period is known; only then is a measurement of the yield worth anything.

### 5.5 The tick rate does not beat against the display (disproved 2026-09-15)

The standing theory, from the user and from the first session, was that the game's true 60.000 Hz
ticker beats against a display that is really 59.9xx, and the stutter is the two crossing. It is
wrong on this hardware.

Measured from the presentation engine every frame, in gameplay, six readings:

| Flip interval | Rate |
| --- | --- |
| 16.6667 ms | 59.9997 - 59.9999 Hz |

The display flips at 59.9998 Hz against a 60.0000 Hz tick. That is two ten-thousandths of a hertz,
a drift of one frame every **eighty minutes**. It cannot produce a jump every few seconds, so the
`Frame Pacing = Display` path has nothing to correct and was not run.

An earlier figure of 59.755 Hz was **an artifact and should be ignored**. The period was only being
sampled at moments when the lock yield fired, and the yield fires on the beat, so the signal was
being sampled in step with itself. Its values swung between 16.674 and 16.790 ms on the same five
second cycle as the yields, which is the fingerprint of aliasing. The "independent" corroboration
offered at the time, a beat loop of about 4.5 s, came from the same biased source.

**Rule taken from this: a periodic measurement must never be sampled on a schedule derived from the
thing being measured.**

## 5.6 The real fix: the game never blocks (built and confirmed 2026-09-15)

**This is the cause.** Everything in 5.1 to 5.5 reduced work or moved waits around. This removes the
reason the game was waiting at all, and the user's verdict on the first run was "it ran incredibly
smoothly, that was it all along."

Peace Walker spins in three places where it should block:

| Loop | Site | What it does |
| --- | --- | --- |
| Render thread | `+17BD0`, sleep at `+17CCF` | reads the handoff byte at display`+0x32c0`; when no frame is ready calls `Sleep(0)` and reads it again |
| Ticker | `+76020`, sleep at `+76051` | compares elapsed against the period; when the tick is not due raises the timer resolution, sleeps the **whole** milliseconds left, drops the resolution, loops |
| Message pump | `+77610` | `PeekMessageA`, then `Sleep(1)` |

The ticker is doubly wrong. Its remaining time is truncated to whole milliseconds, so the last
sub-millisecond of every tick degenerates into `Sleep(0)`; and `timeBeginPeriod`/`timeEndPeriod`,
which take a system-global lock, run on **every iteration** of that spin.

### The measurement

Switched live, in one spot, in one session, with nothing else changed:

| Busy wait fix | Process CPU |
| --- | --- |
| Off (the game's own spin) | 136.3%, then 119.3% of one core |
| Render thread waits on an event | 21.5%, then 27.7% of one core |

About a full core given back, roughly an 80% cut. The counters show the mechanism, not just the
outcome: per 5 s the render thread performs ~2530 waits of which **exactly 300 are satisfied by the
event**, which is 60 a second, one per frame. The rest are the 2 ms safety timeout expiring
harmlessly. With the ticker included it sleeps 300 times per 5 s at 15.93 ms each.

### Isolated: the busy wait fix is the whole fix

Run with the **frame skip governor back at the game's own default**, the **state object cache off**,
frame pacing and present sync the game's own, and the handoff event as the only change to the frame
loop. Render scale 16 and 4x MSAA as usual.

Result: a **flat 16.6 ms frame time at a solid 60**, the first flat graph of either session, judged
in gameplay by the user.

Consequences:

- The **frame skip governor override is not needed for the stutter**, though it stays as a user
  option. Every measurement before this run had it engaged; it was never the fix. Note what it
  actually does: it hooks only the **raise** at `+78462` and skips that store, so a long frame can
  no longer promote the wait count, while the explicit set at `+78070` that paces 30 fps menus and
  movies and the lowering path at `+7843C` are left intact. It turns the governor **down, not
  off**.
- The **state object cache is a performance feature, not a stutter fix**. It removes 350 redundant
  device state creations a frame and about 8% of the game thread's executed CPU, which is worth
  having on slower processors, but it is not load-bearing for the pacing.
- The remaining work is promotion: this fix currently lives in `draw_census.cpp`, which is **Lab
  only**. It has to be reachable from the Release build.

### How it is done

`Busy Wait Fix`: 0 off, 1 the render thread, 2 also the ticker, 3 also the message pump (**level 3
is not written yet**; it currently behaves as 2). Every hook is installed at start-up and reads an
atomic level, so `busywait N` switches behaviour live and the two can be compared in one session.

For the ticker we follow MGSHDFix's approach for MGS2 and MGS3: compute the time left, sleep it on a
`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer less a **1 ms margin** left for the game's own loop to
spin out, capped at **50 ms** so a bad clock read cannot park the game.

For the render thread we deliberately **do not**. It is not waiting on a clock, it is waiting on the
game thread, and Peace Walker publishes the frame with a single store at `+17683`
(`mov byte [rbx+0x32c0], 1`). So there is an exact point to signal from, and a blind sleep would only
delay the frame. We create one auto-reset event, set it just after that store, and wait on it at
`+17CCF` with a **2 ms timeout**, so a signal somehow missed costs one timeout and the loop then
behaves exactly as it does today. It cannot hang. MGSHDFix creates no synchronisation objects for
its games because they offered no such publication point; we have one, so we use it.

**Neither hook redirects execution**, which avoids depending on safetyhook's `rip` semantics. The
render thread's `Sleep(0)` is left to run after our wait, where it is a cheap yield. The ticker's own
sleep is neutralised by setting its elapsed register equal to its period register, so the length it
computes comes out zero; that register is recomputed from the clock at the top of every iteration,
so clobbering it is safe.

### The message pump (level 3, built 2026-09-15)

`PeekMessageA` is inline-hooked in user32 itself, and when it returns false and the level is 3 we
call `MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT)`. That is MGSHDFix's fix for MGS2
and MGS3 verbatim, including the 1 ms timeout. A `Sleep(1)` cannot be woken by an arriving message;
this can, and costs the same millisecond when nothing arrives. Measured at ~1820 parks per 5 s.

Hooking in user32 rather than on the import means every message pump in the process goes through it,
overlays included. The level gate is the only thing that decides who waits.

### All four levels measured

| Level | What is on | Process CPU |
| --- | --- | --- |
| 0 | the game's own spins | 122.1% of one core |
| 1 | render thread event | 26.8% |
| 2 | + ticker timer | 36.9% (menu noise) |
| 3 | + message pump | 23.6% |

At level 3 the render thread's harmless 2 ms timeouts fall from about 2200 to about 150 per 5 s,
while the 300 event-satisfied waits per 5 s are unchanged.

### Signature note

The site check initially failed because the `xor ecx, ecx` at `+17CCF` is encoded **`33 C9`**, not
`31 C9`. Read the bytes, do not assume the encoding.

## 5.7 The Codec and voice-line hitch: a cold archive read, fixed by warming (2026-09-17)

**Symptom.** A single frame hitch whenever a new voice line starts (Codec calls, co-op voice lines).
Testers reported the game otherwise "exceedingly smooth" after 5.6.

**Cause.** One synchronous `ReadFile` on the game thread: 20 KB from a voice archive taking
**9.7-10.2 ms**, in a tick that blocked 13.5 ms. The frame created nothing, mapped a normal amount,
spent 0.25 ms in Direct3D; the wait caller table was empty because a synchronous read blocks in the
kernel without calling any wait function. Confirmed off-CPU: 0.14 ms of executed cycles across a
9.86 ms read.

**Ruled out by test, in order.** Defender (excluded the whole game folder: same file, same 9.82 ms).
Scheduling (game thread at highest priority: read still 9.86 ms, though the tick then fit the budget
at 15.10 ms, which is why the *hitch* disappeared while the *read* did not; priority helps only
because the tick sits on the 16.67 ms boundary). The drive waking from a low-power state: this was
my working theory and it was **wrong**; it rested on "the same file costs the same twice", which is
simply what a cold *region* costs when every voice line lands on a different offset of a 79 MB file.

**Proof.** Warming one archive from outside the game made it vanish from the slow list while other,
cold archives still cost 10 ms. Warming every voice archive, then playing: **not one voice-archive
read over 1 ms across 21 active windows**, every longest read 0.12-0.22 ms and fully executing (a
memory copy); gameplay at a flat 60; the user saw no stutter. So the game reads *through* the file
cache, and residency is the whole story.

**Why the first attempt "did not work".** Mode 1 warms an archive when the game opens it, and the
game opens these before our import hook exists, so it warmed one 0.1 MB file and never touched a
voice archive. The negative result was an instrumentation gap, not evidence. Recorded here so the
"it made no difference" line in 5.6-era notes is not trusted again.

**Inventory, corrected.** 134 archives, 8.4 GB (an earlier 886 / 20 GB figure was a PowerShell
`-Include` double count). 6.3 GB of that is `MLG\disc0_rel\ADEMOHQ`, high-quality demo the voice
path never reads. Every archive that ever stalled the game thread was in `EXLANG\disc0_rel` or
`MLG\disc0_rel\ADEMO`; those plus `ms0\<REGION>\DLCVOICE` are what mode 2 warms: **48 files,
1.6 GB**, done **within two seconds of launch at ten percent of a core**, on a below-normal thread.

**Shipping decision (user, 2026-09-17).** `Audio Prefetch` ships **experimental and off by default**:
proven here, not yet soaked on a 16 GB machine, and a behaviour change at launch. Testers who see
the hitch are told to switch on "Voice archives at start".

**Frame-level note.** Windows during Codec screens show exactly 150 of 300 ticks doubled: that is
the game's designed 30 fps for those screens (the explicit wait count at `+78070`), not a stutter.
Gameplay windows in the same session read 60, 60, 61, 60 frames a second.

## 5.8 NVIDIA variable refresh holds the game to 59 a second (found 2026-09-19)

**Symptom.** One 33 ms doubled frame a second at 60 Hz, back on 2026-09-18 with every fix above
still on and working (the busy wait fix still took the processor from a full core to a quarter).
It looked exactly like the original stutter. It is a different cause, and only on this machine:
the same preview build on a second PC (Radeon RX 6900 XT, 5800X3D) was flat, even with
`Busy Wait Fix` off.

**Cause, confirmed by the driver itself and by the user's A/B.** With G-SYNC (variable refresh)
active on a 60 Hz mode and V-Sync on, whether the game's own or the control panel's, the NVIDIA
driver paces `Present`. `NvAPI_D3D_GetSleepStatus` on the game's device reports:

| State | low latency mode | VRR | driver sleep interval | frames a second |
| --- | --- | --- | --- | --- |
| G-SYNC on, V-Sync (game or control panel) | **ON** | active | **16,948 us** | **59.00** |
| G-SYNC on, Vertical sync = **Fast** | ON | active | 16,666 us | 60.00 |
| G-SYNC off, the game's V-Sync | ON | not active | 16,666 us | 60.00, refresh steady at 16.667 ms |

59.00 is the driver's variable refresh ceiling, `refresh - refresh^2 / 3600`: 59 at 60 Hz, 116 at
120 Hz, 224 at 240 Hz. The game's simulation is a fixed 60.000 Hz and cannot run slower, so at 60 Hz
one frame a second has nowhere to go: the handoff drops it (the 33 ms frame) and the display holds
one refresh open to about 20.3 ms (the panel's adaptive-sync maximum frame interval read 20,583 us).
At 120 Hz and above the ceiling is far over 60 and never bites, which is why section 1's table
says a higher refresh rate "fixes" it: it only masks it. The "low latency mode ON" is the driver's
own state; the control panel's Low Latency Mode was off throughout. It is ON in all three rows,
variable refresh or not, so the driver paces this game regardless: the only thing that changes is
the interval, and only variable refresh together with V-Sync selects the 59 ceiling.

**What does not fix it.**

- `NvAPI_D3D_SetSleepMode` with low latency off and no minimum interval, from the plugin, at device
  creation and again whenever the driver was seen with low latency on. The call is accepted and
  ignored: the interval stayed 16,948 us.
- Removing `Maximum pre-rendered frames` from the game's driver profile. No profile or global
  setting was found that switches the ceiling off while variable refresh and V-Sync are both on.
- Everything in the list below, each excluded by its own test before the driver was asked.

**What fixes it.** G-SYNC off for the game, or Vertical sync = Fast with G-SYNC left on (the user:
"feels good and runs well"). AMD's FreeSync shows no such ceiling. The shipped option is
**`NVIDIA Fast Sync`** (Config Tool, Performance; off by default; `pw/src/features/nv_profile.cpp`
and `vrr_compat.cpp`):

- At start-up, before the game creates its device, the plugin finds the driver profile that matches
  the running exe (`NvAPI_DRS_FindApplicationByName` with the full path), or makes one named
  "METAL GEAR SOLID PEACE WALKER (MGSPWEnabler)", and sets Vertical sync (`0x00A879CF`) to Fast
  (`0x18888888`) in that profile only. The global setting is never written.
- What it replaced is recorded in `MGSPWEnabler.nvidia.state` next to the settings file. Off with
  that file present puts the value back (or removes it, and removes a profile made from here if it
  holds nothing else) and deletes the file. Off with no such file returns before `nvapi64.dll` is
  even loaded, so a Fast value the user set themselves is never touched. If the value is no longer
  Fast when it is switched off, it was changed elsewhere and is left alone.
- `nvapi64.dll` is loaded from System32 at run time, by `nvapi_QueryInterface` ids, no SDK. On AMD
  and Intel the load fails and nothing else happens. A translation layer that answers the query but
  lacks the profile entry points is treated the same way.
- Cost: about 135 ms once at start-up while the option is on (loading the driver's settings store);
  nothing per frame.
- With the option on, the log carries the driver's own status at device creation and 46 s in
  (`PW vrr:` lines). 16,666 us means the driver took it for that start. `NVIDIA Pacing Report`
  (Lab tab, both builds) logs the same lines without the option, plus 106 s and 180 s.
- `nv_profile.cpp` uses nothing of the plugin, so it also builds into a console test that runs a
  **dry run** against the real driver: everything happens inside a settings session that is
  discarded instead of saved. The profile creation path was verified that way (the exe with no
  profile returns -166, a profile is made, the value reads back as the profile's own, and the exe
  is found again in the same profile), without touching the store and without starting the game.

**Excluded on 2026-09-18 and 09-19, each by test, so none is retried:** the Steam update to 1.3.2
(a verified 1.3.1 copy from DepotDownloader stutters the same, with the current plugin and with the
2026-09-16 plugin); the port of the sites to signatures; MGSPatriotFix and its launcher patch;
render and output resolution; window mode; a reboot; the GPU power state (pinned at full clocks,
about 30% load); the NVIDIA and Steam overlays, RTSS, FanControl; Windows' graphics toggles;
RenderDoc (it left no registry key; the only key written on 09-16 was removed the same day);
a Defender exclusion; processor affinity and the all-core burst at launch; the busy wait level
(0, 1, 3: amplitude changes, the once-a-second beat does not); the frame-skip governor (off: the
hitch stays, smaller); a clock mismatch between the game and the display (both read 60.000).

**Not explained.** G-SYNC was on and the graph was flat for days before 2026-09-18. Nothing the
user did changed that night besides the game update, which is excluded. The NVIDIA app updated
itself in the background at 09-17 23:33, 09-18 01:11 and 01:47; whether that changed the driver's
behaviour is unproven. The driver went 616.56 to 616.92 on 09-18 at 23:22, after the onset.

**The per-game route works on the launch that writes it (2026-09-19 11:21).** With the global
Vertical sync back at "Use the 3D application setting", the plugin wrote Fast to the game's profile
at start-up and the driver reported 16,666 us with variable refresh active 46 s later, in the same
process. No second start is needed. The user's graph was flat. Switching it off was verified the
same way at 11:57: the next start logged "put back to not set (follows the global setting)" and
the state file was gone.

**Fast Sync does not buffer or discard anything here (PresentMon 2.5.1, whole session, Release
build, 2026-09-19 11:35; `C:\mgspf_tools\pw\presentmon`, `capture.ps1` and `analyze.py`).** 104 s of
gameplay, 6240 presents:

| Measure | Value |
| --- | --- |
| Presents, shown, dropped | 60.010 a second, all 6240 shown, **0 dropped** |
| Present to screen | mean 6.38 ms, 4.8 to 8.3 ms |
| Present to GPU finished | mean 6.38 ms: **the same number**, frame by frame |
| Between display changes | mean 16.667 ms, 15.4 to 17.7 ms, none over 25 ms |
| Inside `Present` | mean 0.77 ms, max 1.1 ms |
| Mode as PresentMon sees it | Hardware: Independent Flip, sync interval 0, tearing allowed |

Every frame reaches the screen the moment the GPU finishes it, so nothing waits in a queue: a
three-frame buffer would read 33 to 50 ms. Nothing is discarded because the game never renders
faster than the display: Fast Sync only throws frames away when the application outruns the
refresh, and this one is held to 60 by its own clock. What Fast Sync changes is that the game's
`Present(1)` reaches the system as an immediate present (the game's own value, sync interval 1
without tearing, is what PresentMon recorded during the few seconds the window was composed), and
with variable refresh active the panel follows each frame as it arrives. The 60 comes from the
driver's 16,666 us sleep and the game's tick, not from waiting for a refresh.

In the same session: a 4.3 s stretch of "Composed: Flip" (351 to 355 s) in which 248 of 254 frames
were reported dropped and the rest took 31 ms to the screen: the user had alt-tabbed. Menus and
loading ran at 30 a second as usual.

**Baseline, same day 11:57: G-SYNC off, the game's own V-Sync, Fast Sync unticked** (49 s of
gameplay, 2940 presents; the user's graph: stable 60). Neither mode buffers and neither drops:

| Measure | Fast Sync + G-SYNC | V-Sync, G-SYNC off |
| --- | --- | --- |
| Presents a second, dropped | 60.010, 0 | 60.021, 0 |
| Mode as PresentMon sees it | independent flip, sync 0, tearing allowed | independent flip, sync 1 |
| Inside `Present` | 0.77 ms | 0.13 ms |
| Present to GPU finished | 6.38 ms | 6.12 ms |
| Present to screen | 6.38 ms (4.8 to 8.3) | 10.85 ms (9.5 to 12.1) |
| Finished frame waits for the refresh | 0 ms: the panel follows the frame | 4.73 ms (2.7 to 7.7) |
| Between display changes | 16.667 ms, 15.4 to 17.7 | 16.666 ms, 16.48 to 16.85 |

So the user's reading holds: with a game that cannot outrun the display, Fast Sync has nothing to
buffer or discard, and it behaves as "present at once and let variable refresh show it", about
4.5 ms sooner than waiting for a fixed refresh. Under plain V-Sync a frame is still shown at the
first refresh after the GPU finishes it; there is no queue there either.

**Why this game has no V-Sync cushion, in any mode.** The usual picture of V-Sync (two or three
frames queued, the oldest shown, so a late frame is covered by an older one) is what happens when a
game renders as fast as it can and is throttled by `Present` blocking: the queue fills in the first
few frames and stays full. Peace Walker is not throttled by `Present`. Its game thread runs off its
own 60.000 Hz timer, so production equals consumption and the queue keeps the depth it started
with, which is empty. The baseline shows it: 0.13 ms inside `Present` (it never blocks, so the
queue is never full) and 10.8 ms from `Present` to the screen (under one refresh; one or two older
frames ahead would read 27 to 45 ms). The title sequence of the same run, where `Present` does
block (9.9 ms inside, 17 ms to the screen), is still only one refresh deep. So a frame that misses
its refresh is always a visible repeat here; that is the engine's design, not something a sync mode
changes. The same arithmetic explains the 59-a-second hitch: 16.948 - 16.667 = 0.281 ms a frame,
the 0.27 ms a frame by which `Present`'s return was measured creeping, and after about 59 frames it
is one whole frame that a fixed-rate game cannot absorb, so the handoff drops it.

One more number from the baseline: present-to-screen fell steadily through the whole session, 18 ms
at 70 s to 9.4 ms at 230 s, a straight line of 53 us a second (53 parts per million between the
game's tick and the panel's fixed refresh: 60.000 against about 59.997). That is far too small to
be the once-a-second hitch (section 5.5 stands), but it means that on a fixed refresh the margin
between "GPU finished" and "refresh" runs out about every 315 s, and one frame should then be shown
twice. **Predicted from the slope, not observed:** the run ended with 2.7 ms of margin left. With
variable refresh there is no grid to drift against.

PresentMon's `--terminate_on_proc_exit` exited about five minutes after the game, and it holds the
CSV locked until then. It runs elevated with no window, so it cannot be stopped from the harness.

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
- **A saw-tooth on the frame-time graph has only ever been seen with a Lab build in the scripts
  folder (2026-09-19, and once before).** With variable refresh off, about one `Present` in five or
  six blocked until the next refresh and the overlay drew a regular comb; it came and went within a
  session and was not felt in motion (the game thread stayed on schedule, one frame per refresh).
  The Lab build of that night logged a line, flushed to disk, for every `Present` over 8 ms on the
  render thread, and hooks every context call. An explanation by launch phase (the block length
  equalled the distance from `Present` to the next refresh: 7.5 to 9.3 ms in the run with the comb,
  0.3 ms and about 0 ms in two flat runs) was drawn from three launches and is **unproven**. Rule:
  do not judge pacing by eye, or build a fix for a pacing artefact, while a Lab build is deployed;
  confirm on the Release build first.

### 8.y The start-up crash: solved with a debugger, 2026-09-16

**Cause.** `UiBias`'s lock was taken from functions reached by Direct3D hooks, and those hooks are
re-entered from inside calls we are already servicing. Two paths were caught: the **Steam overlay**
draws from within the game's `Present` and sets shader resources, re-entering the census hook, which
calls `UiBias::Active()`; and the runtime calls back during `Unmap`, re-entering `OnConstantUnmap`.

On a plain `std::mutex`, that re-entry is **not a hang**. MSVC throws `std::system_error`
("resource deadlock would occur"), which unwinds into overlay and driver frames that carry no C++
handler, reaches the **game's own** unhandled exception filter at `+75D10`, and becomes
`terminate` -> `abort` -> `__fastfail(FAST_FAIL_FATAL_APP_EXIT)` at `+9594BD`. A fast fail bypasses
user-mode exception dispatch, so no handler of ours could ever see it and the log simply stopped.

**Fix:** the bias lock is recursive, because re-entrancy is inherent to hooking; and `Active()` is
lock-free, since "is anything biased" is one bit read from a hot hook. **Every lock in the main tree
is a plain `std::mutex` again** — with nothing masking anything, the game starts cleanly three times
out of three, where the same build crashed three times out of three before.

**What cost two sessions, and why.** Symptoms moved whenever unrelated code changed, which looked
like memory corruption but was only layout shifting when the overlay first drew. Ruled out by test,
not by argument: MGSPatriotFix (crashes with and without), heap corruption (full page heap found
nothing), our census and cache locks (owner records read zero at the throw), the display critical
section features (compiled out, still crashed), memory pressure, and a same-name global collision
(both are in anonymous namespaces).

**The lesson: reach for a debugger first.** Every in-process instrument perturbed the layout and
moved the fault; four attempts died that way. WinDbg named the culprit on the first run, because it
can walk out of a safetyhook stub, which has no unwind data, and our stack scanners cannot.
AddressSanitizer is **not** an option here: an injected ASI that imports its runtime fails to load
with module-not-found.

**How to do it again:** `cdb -p <pid> -cf script -logo out.txt`, script being `.sympath <bin\Lab\pdb>`,
then `sxe -c ".echo ===CPP-THROW===;kv 30;gc" eh`, then `g`. Do **not** use `.symfix` or
`.reload /f`; the symbol server download stalls past the crash.

### 8.x A probe left ungated is a regression, not a probe

`SampleDisplayPeriod` was added to feed the tick rate test and called from the `Present` hook with
**no guard at all**. It ran on every frame of the "quiet" build that was handed to the user as the
known-good configuration, calling `GetFrameStatistics` in the present path. The user spotted it
immediately from the frame time graph: "looks like you still left some stuff on, it was not doing
this before in this configuration."

Every other addition that session checked its own switch first. This one did not, and it undid the
night's work in the one build that was supposed to prove it.

**Rule: anything added to a hot path is written with its guard as the first line of the function,
before the body exists. Then audit the diff for hot-path calls and confirm each one's guard.**

### 8.z The real cause of the moving symptoms: an out-of-bounds write (2026-09-16)

Two sessions of faults that jumped whenever unrelated code changed were **one stray pointer write**.

`ContextHooks::original` held `kSlotCount = 80` entries, and the census stores each original vtable
entry **by its slot index**. It hooks `kCtxFlush = 111` and `kCtxFinishCommandList = 114`. So every
device creation wrote two 64-bit pointers **31 and 34 slots past the end of the array**, into
whatever globals the linker had placed after it.

**How it was finally caught.** The canvas globals are `int g_UnitsW = 480, g_UnitsH = 272`, and
`Canvas::Configure` refuses 480x272 outright, so nothing in that code can change them. They held
32763 and 1890641920: as adjacent integers, the two halves of `0x7FFB70B20000`, a DLL address. No
dimension calculation produces that. A **data breakpoint** on those eight bytes named the
instruction on the first run: `mov qword ptr [rsi+rdx*8],rcx` in `InstallContextHooks`.

With the units no longer 480x272, `Canvas::Active()` returned true at 16:9, the post-chain quads
were rewritten against a meaningless size, and the fullscreen effects broke with a
direction-dependent flicker. Note the distinction the user drew: at 16:9 the canvas **is** still
scaled, by an integer, in both axes; what should never run is the canvas module's **unit**
rewriting.

**Fixed** by sizing the array to the highest slot hooked rather than the number of slots used, plus
a `static_assert` per slot so a future index above the bound is a build error. `render_hooks.cpp`
was checked and was never affected: its highest slot is 57 against the same 80, which is why the
Release build and the preview package were always sound.

**This retroactively explains** the mutex that reported an owner our own records said did not exist
(8.y), and every instrument that moved the fault instead of finding it. The lesson from 8.y stands
doubled: **when symptoms move with code layout, stop instrumenting and put a data breakpoint on the
corrupted bytes.** Both root causes this project has had were found by a debugger on the first run,
after hours of in-process probing had failed.

## 9. Testing rules

- `boot.ps1 -LabConfig` reads `MGSPWEnabler.lab.settings`. Regenerate it from the user's
  `MGSPWEnabler.settings` before every launch and change only the key under test. A stale lab file
  once produced a run with no supersampling that the user had to diagnose from the picture.
- Check in with the user before launching and before closing while they are testing.
- Every launch needs the user's go for that launch, Release or Lab. A run with nobody at the PC (or
  the monitor off, which switches variable refresh off) is not data.
- `tasklist` truncates the game's image name: match on `METAL GEAR`.
- Compare within one session and one spot in the game. Cross-session comparisons leave room for
  scene differences, which is how the first Fast Sync comparison nearly went wrong.
- The user's frame-time graph is the judge of the picture; the logs are the judge of the cause.
