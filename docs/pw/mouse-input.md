# Peace Walker: mouse input and the road to true mouse aim (started 2026-09-19)

**Goal (the user's, 2026-09-19):** true mouse aim. Mouse movement goes straight to the camera's
yaw and pitch, one to one, in every camera mode. The input "feels bad regardless of camera mode or
button layout", with a lot of filtering on it.

**Status:** the game's side of the mouse is read and understood up to the point where the engine
asks for it. What the engine does with it after that (the part that is felt) is **not located
yet**. Nothing below the line "Not known yet" is a finding.

RVAs are for the 2026-09-18 build (1.3.2, build 25294658), read from the harness's dump
`C:\mgspf_tools\pw\pw_text.bin` with `sites2.py` (run it with `C:\Program Files\Python310`; the
default Python 3.8 has no capstone).

## 1. The game already uses Raw Input

Adding raw input would change nothing: it is there.

- Imports: no DirectInput, no XInput, no GameInput. From USER32: `RegisterRawInputDevices`,
  `GetRawInputData`, `GetRawInputBuffer`, and `GetCursorPos` / `SetCursorPos` / `ClipCursor` /
  `ShowCursor` (a cursor confinement routine at `+3A400..+3A6B5`, not read in full; it is not the
  look path).
- `+7763C`, right after `CreateWindowExA` (`+775E9`): two devices are registered, both with flags 0
  (so input arrives only while the window is in the foreground): `0x60001` = usage page 1, usage 6,
  the keyboard (built by `+3A1B0`), and `0x20001` = usage page 1, usage 2, the mouse (`+3A370`).
- Dispatch at `+764E0` (and the `GetRawInputData` path at `+768B2` / `+768C2`): `dwType` 1 goes to
  the raw keyboard handler `+3A1D0`, `dwType` 0 to the raw mouse handler `+3A390`, each given the
  data at `+0x18` of the `RAWINPUT`. **The device handle is never looked at**, so movement
  injected with `SendInput` (which arrives with a null device handle) takes the same path as a
  real mouse.

## 2. From counts to the engine

```
+3A390  raw mouse handler(RAWMOUSE*)
          only usFlags == 0 (relative movement)
          accumX += lLastX / 256          float at +15FF398, constant 1/256 at +E14890
          accumY += -lLastY / 256         float at +15FF39C
          wheel  += usButtonData          int   at +15FF3A0 (when RI_MOUSE_WHEEL)

+3A850  per-frame device update(device*)
          keys: 0x101 entries of pressed / held / released from the key table at +15FF290
          device+0x848 = accumX, device+0x84C = accumY, device+0x844 = wheel direction (0, 1, 2)
          accumulators zeroed
          then a state handler through device+0x850[device+0x870]

+3A9F0  value getter(device*, code) -> float, always clamped to 0..255 (+E14A10)
          code < 0x10000          a key: 255 while down
          0x10001 .. 0x1000D      the mouse, by a jump table indexed from 0x10001:
            0x10001, 2, 4, 5, 6   buttons: 255 while down
            0x10008               Y+   max(latchedY, 0)
            0x10009               Y-   -latchedY when negative
            0x1000A               X-   -latchedX when negative
            0x1000B               X+   max(latchedX, 0)
            0x1000C, 0x1000D      the wheel's two directions: 255 for that frame
```

So the mouse reaches the engine as **four analog buttons**, each a float from 0 to 255 for the
frame, the same currency as a key or a pad button. The getter has no direct callers: it is a
virtual method, so whatever polls it (the binding layer) has to be found at run time.

## 2a. One level up: bindings and actions (read 2026-09-19)

The getter is polled from `+2D35C` and `+2D3B7`, both inside **`+2D300`, a binding's evaluate**:
a binding holds a primary input code (`+0x20`, device `+0x28`) and a secondary one (`+0x38`,
device `+0x40`), asks the getter for each through the pointer at `+0x10`, and returns the larger,
0..255. A binding flagged digital (`+0x30` / `+0x48`) treats anything under **96** as not pressed.
It is called from **`+2C720`, an action's update**, for each of two players: the largest value
over the action's bindings becomes the action's current value; per player, at
`action + 0x68 + player * 0x24`: `+0` current, `+4` previous, `+0x10` the value reported to the
game (a key-repeat rule applied), `+0x14` / `+0x18` press and release timers, `+0x1C` the source
kind. The update runs from `+2ADC9`, from the main loop (`+77266`, `+764CD`). So each camera
direction is an action holding 0..255 for the frame. The default key configuration is built by a
12 KB static initialiser at `+D800` from hashed action names; the table of the four direction
codes sits at `+D29000`.

## 2b. First measurement (2026-09-19 12:46, Lab build, Fast Sync + G-SYNC, free camera)

Six injected patterns (`C:\mgspf_tools\pw\mouse\`, log `probe_run1.log`). The user watched: the
camera turned, and its speed followed the injected speed up through the sweep.

| Check | Result |
| --- | --- |
| Counts injected against counts the raw handler received | identical in all six (1997 / 1997 ... 31787 / 31787) |
| Getter's answer against the latched value | equal on every frame: nothing between them |
| The 0..255 clamp | never near: 16000 counts a second peaked at 1.44 |
| Polls per frame | X directions 4 each, Y directions 5 each; the same while aiming (right mouse held) |
| A steady 1000 counts a second, per frame | mean 16.6, **min 12, max 22**, standard deviation 2.4 (ideal: 16.67 every frame) |
| Age of the newest event at the latch | mean 3.0 ms, up to 7.8 ms; the first event after a latch is handled 2.2 ms later on average, up to 6.3 ms |
| Latch to latch | 16.666 ms, 15.7 to 17.5 |

Two findings. The path is clean up to the getter: no loss, no dead zone, no smoothing, no cap. And
the per-frame sample of a perfectly steady movement varies by about a third, because events are
handled in bursts rather than as they arrive; if the camera turns at a rate proportional to the
frame's value, that is turn-speed noise on every frame. How much of the burstiness is the game's
message handling and how much is Windows delivering injected events is **not separated yet** (a
real-mouse run will tell). The values themselves are tiny (a brisk movement is 0.07 of 255), so a
large multiplier sits downstream. History the user recalled: at launch the game misbehaved with
mice polling above 250 Hz and was patched; `GetRawInputBuffer` may be that patch (unverified).

## 2c. Second run (13:02): the actions, the key-repeat value, and a real mouse

- **Sixteen action objects (the probe's table was full) ask for a mouse direction.** An action
  has no vtable; its head reads `+4` a hashed name, `+8` a hashed group. Three groups were seen:
  `0x1512DE` (Y+ `0x480BDC`, Y- `0x26FC6E`, X- `0x2AD254`, X+ `0xBC4D79`), `0xF09` (ten actions,
  among them X+ `0xCC5EB9` and `0x97FA92`, Y+ `0x6A8BFC`), `0x43A215` (Y+ `0x942735`, Y-
  `0x94619E`). In gameplay three X+ actions carry the same value every frame.
- **The reported value (`+0x78`) is a menu auto-repeat**, not the camera's input: it passes the
  value on the first frame, nothing for the next 18, then one frame in six. Hardware breakpoints
  on it (four actions) caught only the action update's own write at `+2C85B` while the camera
  turned. The camera therefore reads something else; the current value (`+0x68`) is watched next.
- Hardware data breakpoints set from inside the process (debug registers on the game thread, a
  vectored handler) work: the copy protection does not object, the game kept running.
- **A real mouse shows the same burst handling as the injector**, so it is the game's loop and
  not the injection: newest event 2.6 ms old at the latch on average (11.3 ms at worst), first
  event after a latch handled 2.5 ms later (16.6 ms at worst). About 3 ms of delay and some
  turn-speed noise: real, but not the latency that is felt. The user's mouse reports at up to
  ~8000 events a second (135 in one frame), and the game takes each as its own window message.

## 2d. The chain to the camera (runs 3 and 4, 13:11 and 13:19, plus the dump)

**Found with hardware breakpoints on the actions' current value, then read in the dump.**

1. **The look-input routine, `+73D37E .. +73EC50`** (5.5 KB, called down `+764CD > +7726B >
   +79C34 > +16756E > +152529 > +EF9D6 > +44D00D > +273706`). It looks actions up by hashed group
   and name through `+2B010(manager at +1596A68, group, name, player)`, which returns a pointer to
   the action's current value (source kind at `+0x1C` of that pointer). `rbx` is the player's
   input state, a virtual pad.
   - Group `0x1512DE`, names `0xBC4D79` (X+), `0x2AD254` (X-), `0x26FC6E` (Y-), `0x480BDC` (Y+):
     `x = X+ - X-`, `y = Y- - Y+`, each through `+4FC50` = `clamp(v / 255 * 127, -127, 127)`.
   - It then finds which of the four is largest and takes its source kind. **Source kind 1 is the
     mouse: the byte at `pad + 0x9B2` is set to 1** (`+73E904 .. +73E92B`).
   - Not mouse: the pad's own stick is added, the sum clamped to +-127, then a **dead zone of 48**
     (`+9C0C0C`) is subtracted, the result **truncated to a whole number** and multiplied by 1.6
     (`+D26B0C`). Mouse: all of that is skipped.
   - Stored: `pad + 0x998` look X (float), `pad + 0x99C` look Y (float), `pad + 0x9A8` magnitude,
     `pad + 0x9A0` magnitude as 0..255 (zero under 0x30), `pad + 0x9A2` angle (16 bit).
   - A second set of names (`0x942735`, `0x94619E`, `0x983784`, `0x68F387`, which also exist as
     mouse-bound actions in group `0x43A215`) is read at `+73E2CD`; when a toggle action
     (`0x3D11EB`) is on, any mouse-sourced value among them, however small, becomes a fixed 97.
     Not executed in free-camera play (its reads never tripped a breakpoint). Purpose not known.
2. **The camera's rotation routines read the pad.** The is-mouse byte `pad + 0x9B2` is tested in
   exactly two places outside the routine above: **`+885839 .. +885A27`** (tests at `+885950` and
   `+8859D5`) and its twin **`+889046 .. +88926C`** (`+889161`, `+8891CF`), each part of a larger
   function (`+885419 ..`, `+888D77 ..`) that also loads `pad + 0x998 / 0x99C`. The camera state
   (`rbx` there) holds rotation **velocities** at `+0xB8` / `+0xBC` and ramp terms at `+0xC0` /
   `+0xC4`. For a stick: `ramp = max(0, ramp + x * k)`, `velocity = (ramp + x) * sign`, clamped to
   a maximum speed. **For the mouse: `|x|` goes through `+509B0`, the ramp term is dropped, and
   the clamp to the maximum speed is skipped.** What follows (`+885A14` on, a constant 300 and the
   velocity at `+0xB8` multiplied by another term) has not been read yet.

So the mouse is a **rate** input everywhere: counts per frame, divided by 256, scaled by 127/255,
become a rotation velocity. That is the design fact behind the feel: the camera integrates a
velocity made from one frame's worth of counts (which the burst handling above already makes
uneven), through whatever smoothing the rest of the routine applies. True mouse aim means adding
counts x sensitivity to the angle itself, in these two routines, when `pad + 0x9B2` is set.

## 2e. The camera routines, read in full (2026-09-19, dump only)

Run 5 first: with breakpoints on both groups by name, the camera group `0x1512DE` was read 120
times a second (twice a frame, by the same two sites of the look-input routine) in every mode the
user tried, aiming down the sights included; the second group `0x43A215` was **never** read. So
one routine feeds every mode, and aiming is not what the second group is for (its use stays
unknown). The look-input routine does not run in menus.

`camA` = `+885360 .. +885D3A`, `camB` = `+888D77 .. +88951E` (same structure; which camera mode
each serves is not established). `rdi` = the player object (the pad fields are its `+0x98x`),
`rbx` = the camera: **`+0xA0` pitch and `+0xA2` yaw as 16-bit angles** (65536 a turn, 0.0055
degrees a unit; the constant 182.039 = 65536 / 360 appears throughout), `+0xB8` / `+0xBC` the
pitch / yaw velocity in those units per frame, `+0xC0` / `+0xC4` the stick's ramp terms.

For the mouse (`pad + 0x9B2` set), per axis:

```
x = pad look value, through +4472B0          only the two invert-axis settings (x -1); no curve
no movement this frame  ->  velocity = 0, ramp = 0          no inertia
|x| = |x| * table[setting]                   +509B0: 50 steps, 712.5 .. 3775 in steps of 62.5
                                             (+D95540), index = the byte at [+10C58A0] + 9, minus 1
velocity = sign * |x|                        the stick's drag, ramp and speed clamp are all skipped
angle16 += (int) velocity                    +885C5E / +885C6F and +8894A3 / +8894B8:
                                             cvttss2si, then add word [cam + 0xA0 / 0xA2]
```

So the turn per count is `127 / 255 / 256 * table = 1.386 .. 7.344` angle units (0.0076 .. 0.0403
degrees), linear, with no smoothing, no acceleration and no cap in these routines. Two other
settings bytes sit beside the camera one: `+8` (the second group's table, identical values) and
`+10` (ten steps 0.5 .. 2.0, applied after x4; not the mouse).

**Defect 1, certain: the fraction is thrown away every frame.** `cvttss2si` truncates toward zero
and nothing carries the remainder. At the lowest sensitivity one count is 1.386 units and turns
the camera by 1: 28% lost; every frame loses up to one unit per axis, always toward "slower", so
slow movement is slower than fast movement for the same hand distance, by an amount that depends
on how the counts happen to fall into frames.

**Defect 2, measured in 2b / 2c: uneven sampling.** The angle step of a frame is proportional to
the counts latched in that frame, and a steady hand gives 12 to 22 where it should give 16 or 17
(events are handled in bursts; the newest is 3 ms old at the latch on average, up to 8 to 11).
In a steady pan that is a step that varies by up to a third from frame to frame: judder.

**Not established: the latency that is felt.** Nothing in this chain delays the mouse by more than
the ~3 ms above. Candidates, none measured: the order inside a tick (if a camera routine runs
before the look-input routine has written the pad, that is a whole frame), smoothing between the
16-bit angles and the view that is rendered, and the game-to-render handoff.

## 2f. Run 6 (13:51): the frame of latency, and the free camera's own routines

**The latency is in the loop order.** The main loop calls the input manager's update (`+2ACB0`, at
`+77261`: the device latch and every action) and then the actor scheduler (`+79850`, sixteen
priority lists, `call rdx` at `+79C32`). Measured on every one of 726 frames: **the look-input
routine stores the look value 16.3 to 16.4 ms after the latch**, 0.4 ms before the next latch. So
the frame's wait sits inside the scheduler, before the player's actor, and the mouse is a whole
frame old by the time it becomes a look value, while 16 ms of newer movement waits unread in the
message queue. (Dispatching pending `WM_INPUT` at the latch itself found nothing to dispatch: the
pump has just run there. It was the wrong place.)

**Neither `camA` nor `camB` runs in the free camera** (0 of 726 frames). The look values and the
mouse flag are copied out of the player by `+40193E` (`+401B6D`: `cam + 0x338` look X,
`cam + 0x33C` look Y, `cam + 0x348` the mouse flag), and the free camera reads the copy:

- **Yaw, `+4041D0`:** `turn = look X * table[setting byte 8] (mouse only, +50950) * (1/128) *
  341.333 * [cam + 0xA0] * 2`, then **`cvttss2si` at `+4042B6` and `add word [cam + 0x36C]`**: the
  same truncation, in the routine that is actually used. (A smoothed value is computed into
  `cam + 0x370` first and then overwritten with the raw turn: no smoothing survives.)
- **Vertical, `+4039F0`:** `cam + 0x374` is a float, moved by `look Y * table * (1/128) * (1/36) *
  [cam + 0xA0]` and clamped to `[cam + 0xB8, cam + 0xBC]`. Nothing is lost on this axis.

**Fixes built (Lab keys, F9 toggles both live):** `Mouse Fraction Carry` at the three truncation
sites, and `Mouse Late Sample`: a hook in the look-input routine, right before it looks up its
four actions (`+73E78A`), dispatches the pending raw input, adds what has accumulated to the
actions' current values (marking their source as the mouse) and clears the accumulators, so every
count is used once, a frame sooner. The action lookup returns a pointer to a static zero block
when input is disabled (`+2B0AF` -> `+15FF210`); the hook never writes to a pointer inside the image.

## 2g. Runs 7 and 8 (14:05, 14:20): both fixes measured, aiming included

Same injected patterns with the fixes off, F9, then on (`mouse\aim_test.ps1`, `analyze_aim.py`).
The user had the in-game sensitivity at its lowest step, which is the worst case for the
truncation. Eight movements each way, free camera and aiming down the sights:

| | fixes off | fixes on |
| --- | --- | --- |
| first count to the camera starting to turn | 24.4 ms (19.5 .. 33.0) | **9.2 ms** (2.8 .. 16.9) |
| last count to the camera's last turn | 23.8 ms | **12.9 ms** |
| turn lost, slow free-camera pan (1.5 counts a frame) | -7.90% | -0.14 .. -0.32% |
| turn lost, free camera at 1000 counts a second | -0.69% | -0.02% |
| turn lost, aiming, horizontal / vertical | -3.77% / -5.4 .. -5.8% | -0.1 .. -0.2% / -0.01 .. -0.12% |

- The exact turn for the same counts is identical with the late sample off and on (-4338.91 units
  both times): it neither loses nor repeats a count. What is left of the delay with it on is the
  time a count waits for the next frame, 0 to 16.7 ms.
- The user, switching with F9: input latency clearly lower, stops more abrupt.
- **Aiming down the sights is `camB` (`+888D77`)**, with both axes truncated. Turn per count there
  is exactly `127/255/256 * table`: 1.386 units at the lowest step (0.0076 degrees); the free
  camera is 4.343 units (0.0239 degrees) at the same setting.
- **Entering aim: no ramp and no filtering in this chain.** Turn per count is 1.386 in every
  quarter second from 150 ms after right mouse goes down. The crosshair jumping about for a second
  or two on entry (the user's observation) is somewhere else: the camera's transition, or an aim
  assist; not established.
- **The raw mouse handler runs on its own thread** (3348 against the game thread 33632), spread
  evenly over the frame. That is why dispatching `WM_INPUT` from the game thread never found a
  message, and the late sample's work is done by reading the accumulators.
- **Still there with both fixes: uneven counts per frame** (11 to 22 for a steady 1000 a second).
  The sampling instant is not the cause. The next measurement is the spacing between handler calls.
- Not checked: whether the rendered view follows the 16-bit angle at once. An injected single
  400-count event, watched by the user (snap or glide), is the next test.

## 2h. Run 9 (14:41): the mouse thread sleeps, and movement arrives in clumps

Spacing between consecutive calls of the raw mouse handler, clean 1000 Hz injection, 1997 events:
**1601 came less than 0.25 ms after the one before, and the gaps between the clumps were 3 to 12 ms**
(72 of 3-4 ms, 228 of 4-6, 81 of 6-8, 9 of 8-12). About five events a wake-up, a wake-up every
~5 ms. That is the uneven count per frame (11 to 22 for a steady 1000 a second) and the 3 ms mean
age of the newest event at the latch.

The pump, on the window's own thread (`+77790 .. +777E8`):

```
loop:  PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)
       if got: WM_QUIT -> leave; TranslateMessage; DispatchMessageA; +765B0 (bulk read of pending raw input)
       if [+107BD6E] -> leave
       Sleep(1)                        <- every pass, message or not
       goto loop
```

One message a pass, then a sleep. (The bulk read after each message is presumably the publisher's
patch for mice polling above 250 Hz.) With `Busy Wait Fix = Everything` our `PeekMessageA` hook
waits up to 1 ms for input when the queue is empty, but then returned "no message", so the message
that ended the wait sat through the loop's `Sleep(1)` as well: wait + sleep + sleep, with each
sleep 1 to 2 ms at 1 ms timer resolution, is the 4 to 6 ms that was measured. Without our timer
hold the sleep would be 15.6 ms.

**Fix built (Lab key `Mouse Prompt Pump`, in `busy_wait.cpp`, only with level 3):** the hook fetches
the message that woke it, and a mid-hook on the loop's `call Sleep` (`+777E2`) turns `Sleep(1)`
into `Sleep(0)`; the hook's own wait keeps the thread from spinning. F9 now toggles all three
fixes; F10 logs and resets the handler histograms.

The single 400-count event went into the yaw in one frame (-1736 units at f=9398, +1736 back at
f=9518): nothing eases the angle. Whether the view eases after it is for the user's eyes; the next
run turns a half circle in one event (`-Pattern impulse -Counts 7545` at the lowest sensitivity).

A dedicated core for that thread (the user's question): it is not being pre-empted, it is asleep,
so affinity would not help; a priority boost is a possible extra once the sleeping is gone.

## 2i. Runs 10 and 11 (15:03, 15:32): delivery by polling rate, each fix alone, and the user's verdict

**The half turn.** 7545 counts in one event (180 degrees in the free camera at the lowest
sensitivity step): -32768 units into the yaw in one frame, +32769 back. The user saw it snap.
Nothing eases the view after the angle.

**Delivery** (`mouse\pump_test.ps1`, `analyze_pump.py`), counts the camera works from per frame:

| input | all off | prompt pump only | late sample only | all on |
| --- | --- | --- | --- | --- |
| 1000 Hz (ideal 16.7) | 16 - 17 | | | 16 - 17 |
| 8000 Hz (ideal 133.3) | 87 - 180 and 102 - 165 (4.7 - 6.9%) | 131 - 136 (0.5%) | 130 - 136 (1.0%) | 130 - 137 (0.7 - 1.3%) |

The user's own mouse set to 1000 Hz or to 8000 Hz made no difference to these (injected events do
not come from it). At 1000 Hz the game's pump keeps pace, one message a millisecond; at 8000 Hz it
reads clumps of about eight, and how many clumps fall before the latch varies: one clump is 6% of
a frame. **Not reproduced in these sessions: the 11 to 22 counts a frame at 1000 Hz of runs 1 to 9**
(same machine, timer resolution 1.00 ms throughout). Working theory, unproven: the pump has two
stable regimes (keeping pace; or emptying the queue and then taking wait + sleep + sleep to come
round), and the prompt pump removes the second by construction. My hook on the pump's `call Sleep`
was checked in the live process: the call is relocated intact and `ecx` stays 1 when the switch is
off, so its presence is not what changed the behaviour. `+765B0`, called after each dispatched
message, is the cursor capture and clip state machine, not a bulk read.

**Each fix alone, the user's own 8000 Hz mouse, aiming (`camB`), horizontal axis.** Jitter = RMS
deviation of a frame from the mean of its two neighbours, as a share of the mean speed; 1000 to
2900 frames per state (`mouse\analyze_jitter.py` does the same from the hotkey lines):

| fraction carry / late sample / prompt pump | jitter of the counts | turn kept (added / exact) | the user, by feel (F6 / F7 / F8, blind to the numbers) |
| --- | --- | --- | --- |
| off / off / off | 24.7% | 99.59% | |
| ON / off / off | 21.9% | 99.99% | "can't notice a huge difference, maybe worse; placebo territory" |
| off / ON / off | 22.4% | 99.68% | "massive latency reduction, immediately feel it" |
| off / off / ON | **6.3%** | 99.60% | "huge reduction in jitter, very apparent" |
| off / ON / ON | **4.65%** | 99.65% | "F7 + F8 felt better" |
| ON / ON / ON | 7.3% | 99.98% | |

So: the **late sample is the latency fix** and does nothing for jitter with a real mouse; the
**prompt pump is the jitter fix**; the **fraction carry changes neither**, and at this user's
speeds (66 to 111 counts a frame from a high-DPI mouse) the truncation it removes is only 0.4%.
Its case is slow movement, low DPI and low sensitivity (7.9% in 2g), not feel.

**Two new observations from the same log, not yet investigated:**
- In the free camera the turn per count is 4.34 in 2600 frames, but **8.69, 13.03, 21.72 and 26.06
  (2x, 3x, 5x, 6x) in 28**. The yaw routine multiplies by `[cam + 0xA0]`; if that carries the
  frame's time step, a hitch frame doubles or triples the mouse turn, which is right for a stick
  (a rate) and wrong for a mouse (a distance). `camB` shows no multiples.
- **Frames with more than 5 counts and a turn of exactly zero: 74 of 2700 in the free camera, 209
  of 12550 aiming.** Whether those counts are lost or the camera is legitimately blocked in those
  frames (a transition, for one: the user reported the crosshair jumping about on entering aim) is
  not known.

## 2j. Vertical against horizontal (the user: "up and down feels like the old behaviour")

**Aiming (`camB`): the two axes are treated identically.** On the user's own movement: 1.39 units
per count on both axes (12341 of 12550 horizontal frames, 3777 of 3829 vertical), a turn of zero
in 1.7% and 1.4% of frames, and at the same speed band (10 to 40 counts a frame) the vertical
counts are no more uneven than the horizontal ones (residual 1.2 to 3.1 counts against 3.9 to
6.9). Both axes get the late sample and the carry.

**Free camera: vertical is not a rotation at all.** `+4039F0` moves a float `p` (`cam + 0x374`,
clamped to `[cam + 0xB8, cam + 0xBC]`) by `look Y * table * (1/128) * (1/36) * [cam + 0xA0]`, and
`cam + 0x378 = (int)(p * 3)`. The pose routine (`+404CC0`) then **interpolates the camera's offset
between four presets** at `+F8F8B8`, three floats a level (values from the dump):

| level | | | |
| --- | --- | --- | --- |
| 0 | 1000 | -1400 | 900 |
| 1 | 2000 | -1000 | 900 |
| 2 | 3700 | 950 | 500 |
| 3 | 2250 | 5500 | 0 |

(the first two read as distance and height; not verified), with `p * 3 - level` as the fraction:
a rail from a low camera to an overhead one, in three straight segments of very different length,
so the same mouse distance changes the view's elevation by very different amounts along it. And a
second routine (`+4046CB .. +404769`) eases a value derived from `p` above 2/3
(`obj + 0x8C += (1 - pow(0.3, dt)) * (target - obj + 0x8C)`, 70% a frame). So horizontal is an
angle that follows the counts, and vertical is a position on a rail, non-uniform, clamped, and at
least partly eased. That is the game's PSP camera, not a leftover of the defects fixed above; what
else smooths the camera's position has not been read.

**The rail in numbers** (computed from the table and the measured yaw scale, not yet measured in
the running game; the harness has the chart: `C:\mgspf_tools\pw\mouse\plot\rail_plot.html`). The
pose routine shows what the three columns are: column 1 is the horizontal distance behind the
player (rotated by the yaw, `+404F4D .. +404F72`), column 2 is added to the camera's height
(`+404F80`), column 3 lifts the look-at point (`+404EF6 .. +404F43`). The view's elevation is
therefore `atan2(-height, distance)`: **+54.5, +26.6, -14.4 and -67.8 degrees** at the four
presets. One count moves `p` by `1.767e-4` at the lowest sensitivity step (5659 counts for the
whole rail, if the clamp leaves all of it; the same 5659 counts turn the yaw 135 degrees). Because
distance and height are interpolated linearly and the angle is not linear in them, **the view
turns by 0.011 to 0.038 degrees a count depending on where the camera is on the rail, 0.46x to
1.59x the horizontal 0.0239, and it jumps by a factor of three at each preset** (0.011 -> 0.034
crossing preset 1, 0.012 -> 0.038 crossing preset 2).

Options, none built: (1) make the rail uniform: scale the step by the inverse of the elevation's
slope at `p`, so that a count is a constant change of view elevation; (2) remove the easing on the
values the rail drives; (3) a true orbit pitch in place of the rail, which is a camera redesign
(collision and the presets' distances included), not a patch.

## 2k. Run 12 (16:35): the rail measured, the uniform rail verified, and the four fixes shipped as settings

Measured with slow full vertical sweeps (`mouseail_test.ps1`, `analyze_rail.py`): the rail position
is clamped to [0, 1], the whole rail is 5657 counts (5659 computed), the view goes from +54.4 to
-67.7 degrees, and one count is worth 0.011 to 0.038 degrees along it: the computed chart was
right. **With `Mouse Uniform Rail` on: 0.02372 to 0.02401 degrees a count everywhere (max / min
1.01), mean 0.02386, exactly the horizontal value**, over 5110 counts end to end. The user: "feels
different, maybe better, will take some getting used to".

Since 2026-09-19 the four are ordinary settings in both builds (Config Tool, Performance tab,
"Mouse"): `Mouse Late Sample`, `Mouse Prompt Pump` and `Mouse Fraction Carry` default on,
`Mouse Uniform Rail` defaults off (it changes the feel), `Mouse Vertical Ratio` 1.0. The Lab
build keeps the live switches (F5 to F10, with `Input Probe`) and the telemetry; a Release build
carries neither.

## 3. Not known yet

Everything that is felt is downstream of the getter and has not been read: the sensitivity scale,
whether the four directions are folded into a virtual PSP analog stick (8 bits, dead zone), the
camera's response to that stick (a turn *rate* with easing, where a mouse wants an *angle*), and
any cap on the turn rate. Which of those exist, and how much each contributes, is for the
measurements to say. Present-to-screen is 6 to 11 ms (frame-pacing.md 5.8), so display latency is
not where most of it is.

## 4. Plan

1. **Measure before touching.** Lab key `Input Probe` (`pw/src/features/input_probe.cpp`) hooks
   the three functions above and logs one line per frame with mouse activity: the raw counts that
   arrived (with the performance-counter stamps of the first and last event), what was latched,
   what the getter answered for each direction and how many times it was asked, and, once per
   distinct caller, the return address and a short stack. The harness injector
   `C:\mgspf_tools\pw\mouse_move.ps1` sends relative movement at 1000 Hz (measured: 1.000 ms
   gaps, 4 us of jitter) in set patterns and writes every event with the same clock:
   `step` (constant speed, stop, reverse), `sweep` (100 to 16000 counts a second: the gain curve
   and any cap), `micro` (1 count at a time: the dead zone), `impulse` (one 400-count event: a
   smoothing tail). It aborts the moment the game loses the foreground.
2. **Follow the chain** from the getter's callers: binding layer, virtual pad, camera update, for
   each camera mode (free camera, aiming, first person, scope). Add the camera's yaw and pitch to
   the probe once found, so step 1's patterns become true step-response curves.
3. **True mouse aim:** in the camera update, when the mouse moved, add counts x sensitivity
   directly to yaw and pitch and take the mouse out of the stick path for that frame. Per camera
   mode. Keys: sensitivity (X and Y), invert, and the feature switch, off by default until the
   user has judged it on a Release build.
4. Out of scope and untouched: the menu cursor path, and all aspect ratio, resolution and canvas
   code.
