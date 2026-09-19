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

Measured with slow full vertical sweeps (`mouse
ail_test.ps1`, `analyze_rail.py`): the rail position
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

## 2l. Next: the same treatment for the controller (plan, nothing built)

What is already known about the stick's path, all from the look-input routine (`+73D37E`, 2d) and
the camera routines (2e); none of it has been measured with a controller yet:

- **The dead zone is in the look-input routine, and it is large.** When the largest look value does
  not come from the mouse: the four direction actions give `x = right - left` through `+4FC50`
  (`v / 255 * 127`), the pad's own stick value is added (`+73EA1E`, clamped to +-127), then
  `+73EA7A .. +73EB02` subtracts **48** (`+9C0C0C`) from the magnitude, **truncates to a whole
  number** (`cvttss2si`) and multiplies by **1.6** (`+D26B0C`). So the first 38% of the stick's
  travel does nothing, the rest is quantised to 79 steps, and full deflection comes out at about
  126. A second, smaller threshold sits on the stored magnitude (`pad + 0x9A0` is zero under
  `0x30`). The movement stick goes through its own copy of this earlier in the same routine
  (`+73E5xx .. +73E76A`, angle and magnitude into `pad + 0x98E / 0x994`); not read in detail.
- **The stick is a rate with a ramp, a drag and a cap** (camera routines, 2e): `ramp = max(0, ramp +
  x * k)`, `velocity = (ramp + x) * sign`, less a drag term, clamped to a maximum speed, and
  the free camera's yaw (`+4041D0`) blends 1 - pow(0.5, dt) of the old velocity before it
  overwrites it. That is the designed feel of a stick and mostly wanted; the dead zone and the
  quantisation are what cost precision.
- **The frame of latency is not specific to the mouse.** The whole input update (`+2ACB0`: the
  device latch and every action) runs before the actor scheduler, whose wait comes before the
  player's actor (2f): every input, the pad included, is a frame old when the game uses it.
  `Mouse Late Sample` only re-reads the mouse.
- **The truncation of the turn** (2e) applies to the stick too, in all three camera routines; the
  carry is switched off for non-mouse frames on purpose for now.
- **Not known: how the pad is read at all.** The executable imports no XInput, DirectInput or
  GameInput, and only `SteamAPI_*` core functions; the launcher passes `-ctrltype PS5`. Raw HID
  through the same Raw Input registration, or Steam Input through an interface fetched at run
  time, are the candidates. The value getter (`+3A9F0`) belongs to the keyboard-and-mouse device;
  the pad's device object and getter have to be found the way the mouse's were (the binding's
  device pointer at `+0x28`, then its vtable).

Steps, in order, each measured before the next:

1. **Find the pad's device and its poll.** Probe: for the look and move actions, log each
   binding's device pointer, vtable and code while a stick is held; hook that device's update and
   getter. Deliverable: where the stick bytes come from, at what rate they arrive, and when in the
   tick they are latched.
2. **Measure it.** There is no injector for a pad, so: a steady held deflection and slow sweeps by
   hand, logged per frame (raw stick, the action values, `pad + 0x998 / 0x99C`, the camera's
   turn). That gives the real dead zone in stick units, the response curve, the quantisation, and
   the latch-to-use delay, the way 2b to 2g did for the mouse.
3. **Late sample for the pad.** If the pad is polled (a state read, not events), re-poll it in the
   look-input hook (`+73E78A`) and overwrite the stick's contribution there: the same 16 ms as the
   mouse, with no double-counting problem at all, since a stick is a state and not a sum. If it
   arrives as events on the window thread, the prompt pump already helps it and the late read is
   of the accumulated state.
4. **Dead zone as a setting.** Replace the constant 48 at the two sites (look and move) with a
   setting (say 0 to 48, default left at the game's value until judged), rescaling so that full
   deflection still reaches full speed: `out = (in - dz) * 127 / (127 - dz)` instead of
   `(in - 48) * 1.6`. Drop the `cvttss2si` there (keep the fraction). Worn sticks drift, so zero is
   not a sane default; something like 8 to 12 of 127 is what modern games ship.
5. **Response curve, optional.** With the dead zone small, a slight exponent on the magnitude
   (1.0 = linear) gives fine aim back near the centre without the dead band. Only if step 4 alone
   feels twitchy.
6. **Carry the fraction for the stick too**, once 4 is in: with a small dead zone the slow end of
   the stick produces sub-unit turns that the truncation would otherwise throw away.
7. Same rules as the mouse work: Lab switches with live toggles first, numbers before feel, the
   user judges on a Release build, nothing on by default that changes the feel.

### 2l.1 The controller arrives through Steam Input (the user, 2026-09-19)

The game is played with a DualSense through Steam Input, so whatever is done here applies to any
controller Steam Input presents. Consistent with the imports: no XInput, only
`SteamInternal_FindOrCreateUserInterface` and friends, through which an `ISteamInput` interface
can be fetched at run time (not yet confirmed in the code). Two consequences for the work:
**Steam's own per-game controller configuration sits in front of the game** (its dead zone,
response curve and sensitivity are applied before the game sees the stick), so measurements and
tester reports must state those settings, and the honest baseline is Steam's dead zone set to
none; and the pad is a polled state (`GetAnalogActionData` style), which is the easy case for a
late re-poll (step 3).

### 2l.2 Every known value in the stick's path (RVAs of build 25294658; values read from the dump)

Most of these constants live in `.rdata` and are **shared by unrelated code** (127, 1/128, 0.7, 5
and so on are used all over the executable). Never patch the constant: hook the instruction that
loads it, as the mouse fixes do. "Site" is the instruction to hook.

**Look-input routine `+73D37E .. +73EC50`** (stick and keys to `pad + 0x998 / 0x99C`)

| what | value | constant | site |
| --- | --- | --- | --- |
| action value to stick units | `v / 255 * 127`, clamped +-127 | `+E14A10` 255.0, `+E14A04` 127.0, `+E14A94` -127.0 | `+4FC50` (leaf, called at `+73E7C9`, `+73E824`, `+73E56F`, `+73E581`) |
| sum of actions and the pad's stick, clamp | +-127 | `+E14A94` -127.0 (xmm9), 127 / 128 in xmm12 | `+73EA1E .. +73EA46` |
| **look dead zone** | **48.0** of 127 | `+9C0C0C` | loaded at `+73EA7E`, applied X `+73EA88 / +73EAA7`, Y `+73EAD2 / +73EAED` |
| whole-number truncation after the dead zone | `cvttss2si` | | X `+73EA90 / +73EAAC`, Y `+73EAD9 / +73EAF1` |
| **gain after the dead zone** | **1.6** ((127 - 48) * 1.6 = 126.4) | `+D26B0C` | loaded `+73EAC3`, applied X `+73EACB`, Y `+73EB02` |
| stored magnitude floor | the 0..255 magnitude is zeroed under **0x30** (48) | immediate | `+73EBAC` (`cmp eax, 0x30`) |
| magnitude scale | `length / 127 * 255` | `+E14A10` 255.0 | `+73EB95 .. +73EBA8` |
| look angle, 16-bit | `atan2 * 32768 / pi` | `+9C0604` 32768.0, `+990A18` (double) | `+73E73C .. +73E767` |
| look lock-out after certain states | **60** frames | `+10A5674` (int, `.data`) | written `+73E970`, `+73E9B8`, `+73E9FF`; counted down `+73EA0B` |
| movement stick: same kind of block | centre 128, range check `0x51 .. 0xAF` | `+E14A08` 128.0, `+D27B64` -128.0 | `+73E586 .. +73E5E5`; angle and magnitude to `pad + 0x98E / 0x994` at `+73E76A` |
| mouse-as-button value (second set, toggle `0x3D11EB`) | 97.0 | `+D72BBC` | `+73E502` |
| digital binding threshold | analog under **96** of 255 = not pressed | `+E149F8` | `+2D32E` (binding evaluate `+2D300`) |
| outputs | `pad + 0x998` look X, `+0x99C` look Y (floats, stick units), `+0x9A8` magnitude, `+0x9A0` magnitude 0..255 (word), `+0x9A2` angle (word), `+0x9B2` is-mouse (byte), `+0x98C / 0x98E / 0x994` movement | | stored `+73EBD2 .. +73EC3D` |

**Aimed cameras `camA +885360`, `camB +888D77`** (rate control; `cam + 0xB8 / 0xBC` velocities, `+0xC0 / 0xC4` ramps)

| what | value | constant | site (camA; camB has the same shape) |
| --- | --- | --- | --- |
| stick magnitude normalised | `abs(x) * 1/128`, clamped 0..1 | `+E14894` 0.0078125 | `+88568D`, `+8856E1` |
| speed band, normal | **1.7 .. 12.0** | `+D24644`, `+D2687C` | `+885624 .. +88562C` |
| speed band, other state (`player + 0x14CC` bit 25) | **5.0 .. 38.0** | `+E149A4`, `+9C3EB4` | `+885612 .. +88561A` |
| blend of the band | 0.7 and 0.3 | `+9C0BDC`, `+98F738` | `+885681`, `+8856B7` |
| maximum turn, degrees to angle units | **182.039** (65536 / 360) | `+990F4C` | `+885703`, `+8858F1`, `+885AB7` |
| per-camera speed words | `[[cam + 0x310] + 0x3A / 0x3C / 0x3E / 0x40] * 1/256` | `+E14890` 0.00390625 | `+8854F9 .. +8855BB` |
| drag divisor | **300.0** | `+E14A18` | `+885841`, `+885A14` |
| ramp | `ramp = max(0, ramp + x * k)`, `velocity = (ramp + x) * sign` | | yaw `+8859A3 .. +8859CD`, pitch `+885B55 .. +885B79` |
| speed clamp (stick only) | +- the maximum turn (r14w) | | yaw `+8859DE .. +885A11`, pitch `+885B8A .. +885BBD` |
| "fast turn" flag | abs(velocity) / maximum > **0.9** | `+9C191C` | `+885BC8 .. +885C47` |
| turn truncation | `cvttss2si`, `add word [cam + 0xA0 / 0xA2]` | | `+885C5E / +885C6F`; camB `+8894A3 / +8894B8` |

**Free camera** (`cam + 0x338 / 0x33C` look copy, `+0x348` is-mouse, `+0x36C` yaw, `+0x374` rail)

| what | value | constant | site |
| --- | --- | --- | --- |
| look copy from the pad | | | `+401B6D`, `+401B7A`, flag `+401C69` |
| yaw gain | `x * 1/128 * 341.333 * [cam + 0xA0] * 2` | `+E14894`, `+D27C40` 341.333 | `+404294 .. +4042AD` |
| yaw smoothing (computed, then overwritten) | `1 - pow(0.5, dt)` | `+E148B0` 0.5; dt = `+E9C23C` (int 1) * `+E9C278` (1.0) | `+4041F3 .. +40426D` |
| camera speed factor | `[cam + 0xA0]` = **0.5875** measured | field | |
| yaw truncation | `cvttss2si`, `add word [cam + 0x36C]` | | `+4042B6`, `+4042CA` |
| yaw limit against the player's facing | **0x3FFF** | immediate | `+4042FA .. +40432D` |
| auto-centre | 0.0104167, 0.01, 0.015625, pi/2, 1.3, 5 | `+D27C18`, `+98FCF4`, `+D27C1C`, `+E14910`, `+D24D34`, `+E149A4` | `+404020 .. +4041A0` |
| rail step | `y * 1/128 * 1/36 * [cam + 0xA0]` | `+D27C24` 0.0277778 | `+403A47 .. +403A67` (our hook at `+403A5F`) |
| rail clamp | `[cam + 0xB8, cam + 0xBC]` = [0, 1] measured | fields | `+403AB7 .. +403ACF` |
| rail levels | `(int)(p * 3)`; the keyboard steps by 1/3 | `+E14990` 3.0, `+E148A4` 0.333333 | `+403AD7 .. +403AE3`, `+403A77 .. +403AAF` |
| rail presets | 4 x (distance, height, look-at height) | `+F8F8B8` (`.data`) | read `+404CEA .. +404D9D` |
| rail easing | `obj + 0x8C += (1 - pow(0.3, dt)) * (target - obj + 0x8C)`, for p over 0.666667 | `+9C0BDC` 0.7, `+D27C2C`, `+E148A4` | `+4046BC .. +404769` |

**Sensitivity settings** (object at `[+10C58A0]`)

| byte | getter | table | use |
| --- | --- | --- | --- |
| `+8` | `+84E80` | `+D95610`, 50 floats 712.5 .. 3775 in steps of 62.5 | mouse, free camera (`+50950`) |
| `+9` | `+84E90` | `+D95540`, the same 50 values | mouse, aimed cameras (`+509B0`) |
| `+10` | `+84EB0` | `+D94578`, 10 floats 0.5 .. 2.0 in steps of 1/6, after a fixed x4 (`+E149A0`) | `+50A10`: not the mouse; very likely the stick's camera speed (unverified) |

**Input layer** (shared by every device): key repeat in the action update `+2C720` (step
`[+E9C238]` = 5 a frame, first repeat after 18 frames, then every 6: `+2C833 .. +2C853`); the
input update `+2ACB0` called at `+77261`, before the scheduler `+79850` whose wait precedes the
player's actor (the frame of latency).

### 2l.3 Settings: every stick value a slider (the user's requirement)

All of it goes in the Config Tool as sliders, including the parts of the feel that are wanted, so
that players can tune it; each defaults to the game's own value, so an untouched install plays
exactly as shipped. The tool's numeric field is an integer (`F::Int`, a spin box today; a real
slider control is a tool change to make with this work), so values are percentages or stick
units. Proposed set, to be pruned or extended once step 2's measurements are in:

| slider | range | default (= the game) | replaces |
| --- | --- | --- | --- |
| Stick Look Dead Zone | 0 .. 64 stick units | 48 | `+9C0C0C` at `+73EA7E`, with the gain recomputed as `127 / (127 - dz)` so full deflection stays full |
| Stick Move Dead Zone | 0 .. 64 | the game's (to be read) | the movement block `+73E586 ..` |
| Stick Look Sensitivity X / Y | 25 .. 400 % | 100 | a multiplier on `pad + 0x998 / 0x99C` when the input is not the mouse |
| Stick Response Curve | 50 .. 300 (exponent x 100) | 100 (linear) | applied to the normalised magnitude after the dead zone |
| Stick Turn Speed, minimum / maximum | 25 .. 400 % | 100 | the 1.7 .. 12 (and 5 .. 38) band |
| Stick Acceleration (ramp) | 0 .. 400 % | 100 | the ramp term `x * k` |
| Stick Drag | 0 .. 400 % | 100 | the term divided by 300 |
| Stick Fraction Carry | off / on | off | the truncation sites, for non-mouse frames |
| Stick Late Sample | off / on | off | a re-poll in the `+73E78A` hook |

## 2m. The controller path, established (2026-09-19, static analysis; stopped early for budget)

Seven parallel investigations of the dump, each claim then adversarially re-derived by an
independent agent: 104 claims, 34 verified before the run was stopped (30 confirmed, 4 corrected).
The raw material is kept at `C:\mgspf_tools\pw\mouse\controller_recon_partial.json`. Nothing here
has been measured in a running game yet; everything is read from the executable.

### What the controller path is

**Steam Input is the only gamepad source.** No XInput, no DirectInput, no delay-loads, no other
gamepad strings. `ISteamInput006` is reached through the SDK's inlined accessor (init thunk
`+2A340`, context array `+1063900`, live interface pointer `+1063910`). Exactly three functions
use it: `+2A370` (start-up: `RestartAppIfNecessary(2492660)`, `SteamAPI_Init`, `ISteamInput::Init`),
`+2A3D0` (shutdown) and **`+65740` = `Input::SteamInputWork::Update`**, the per-frame poll.

**The pad is a once-a-frame state poll on the main-loop thread**, not events:

```
main loop +77261 -> input manager update +2ACB0
                      +2AD91  Input::KMInputWork::Update      (keyboard and mouse)
                      +2AD9E  Input::SteamInputWork::Update   (+65740: RunFrame(true),
                                GetConnectedControllers, GetAnalogActionData x2, digital x16)
                      +2ADC4  per-player action update        (+2C720)
                 -> actor scheduler +79850 (+77266) -> ... -> the look-input routine
```

`SteamInputWork` (vtable `+DAD018`, RTTI `.?AVSteamInputWork@Input@@`, instance in the global
`+1596E38`, 0x780 bytes, created with `KMInputWork` at `+4AF10`) holds 16 pad records of 100 bytes
from `inst+0x08` (a connected byte then 24 floats). The action set is **"CommonSet"** with two
analog actions, **`stick_l`** (handle at `inst+0x750`) and **`stick_r`** (`inst+0x758`), and
sixteen digital actions. Every pad axis read funnels through one static function, `+4B120`, called
by the binding evaluate `+2D300`. `Input::ControllerType`: 0 = SteamInputWork (pad), 1 =
KMInputWork (keyboard and mouse), 2 = none.

**A "Stick Late Sample" is therefore possible and needs no new Steam call:** a hook at `+73E78A`
can call `SteamInputWork::Update` through the global and re-read the actions, exactly as the mouse
late sample does. The pad is a state, so there is nothing to double-count.

**The game applies no sensitivity setting at all to pad input.** All 25 call sites of the four
sensitivity scalers (`+50950`, `+509B0`, `+50A10`, `+50A60`) are gated on the input being the
mouse (`[ptr+0x1C]==1 && [ptr+0x20]==1`, i.e. ControllerType == 1). The options record at
`+10C58A0` contains no controller float or axis field. So a plugin stick-sensitivity slider is
purely additive, with nothing in the game to compose with. (`+50A10` is the mouse **zoom step**,
not the pad's camera speed: the recorded guess in 2l.2 is wrong.)

Corrected while verifying (2l.2 said otherwise):

- **The term folded in at `+73EA1E` is not a separate pad-stick read.** It is the **movement (left)
  stick**, computed earlier in the same routine from four more actions in group `0x1512DE`
  (`0x68F387` / `0x983784` X, `0x942735` / `0x94619E` Y), folded into the look axes behind a gate:
  at least one movement axis non-zero, an "allow" flag from three state tests, and a countdown at
  scratch `+0x??` (the 60-frame lock-out). So the look block is fed by **actions only**; it never
  touches `SteamInputWork` directly. The strcodes resolve to names: group `0x1512DE` = "ingame".
- **The dead-zone block `+73EA7A..+73EB02` is skipped when `player+0x9B2` (input is mouse) is set**,
  so it is the pad-and-keyboard path. But that flag is *cleared* at `+73EA4A` a few instructions
  before the gate reads it, so "this can never affect the mouse" is **not** safe as a blanket
  statement: a hook here must re-derive the source itself rather than trust the flag.
- **1.7 / 12.0 and 5.0 / 38.0 are not turn speeds.** They are the low and high limits of a band
  applied to a player state scalar (`player+0x8B0`); the alternate band needs
  `(byte[player+0x14E0] & 7) == 0` **and** `(dword[player+0x14CC] & 0x02000000) != 0`.
  **0.7 and 0.3 are a weight and an offset** on the per-camera turn value, not a blend of the band.
- **The 300.0 drag term is a dead store on most frames.**
- **The four per-camera words** at `[cam+0x310]` (camA) / `[cam+0x2D0]` (camB) have **two** roles,
  not one.
- **The stick already has a response curve**: `g(m) = 0.005 + 0.145 * m^2`, a 2-point table at
  `+D90010` evaluated with the t^2 ease-in handler. The ramp coefficient is a **2-point step**
  (table `+D90030`, `-100.0` below the threshold), not a smooth curve.
- **camB (aim down sights) has no band at all**, and the **free camera has none of this
  machinery**: no band, no per-camera words, no ramp, no clamp, no curve; its stick yaw is a single
  linear term.
- **The fast-turn flag (0.9 at `+9C191C`) is NOT mouse-gated** and touches both paths: a hook there
  would change mouse behaviour.

### Hook sites that survived verification

Each is 4 to 9 bytes, none is a branch target, and each was checked for being reached only on the
path claimed. Every constant involved is shared with unrelated code, so the rule stands: hook the
loading instruction, never the constant.

| tunable | site | what is live there |
| --- | --- | --- |
| look dead zone X | `+73EA88` `subss xmm8, xmm0` (F3 44 0F 5C C0) and `+73EAA7` `addss xmm8, xmm0` | xmm8 = look X, xmm0 = 48.0 |
| look dead zone Y | `+73EAD2` / `+73EAED` (same shape, xmm6) | xmm6 = look Y |
| look gain X | `+73EACB` `mulss xmm8, xmm1` (F3 44 0F 59 C1) | xmm8 = post-dead-zone X, xmm1 = 1.6 |
| look gain Y | `+73EB02` | xmm6 |
| per-axis sensitivity, curve | `+73EB06` `comiss xmm8, xmm7` (4 bytes) — the common tail, after the dead zone and gain, before the clamp and the stores | xmm8 = X, Y in memory; `rbx+0x9B2` distinguishes pad from mouse |
| last chance before publish | `+73EBD2` (9 bytes) | xmm8 = final look X; Y at `[rdi+0x70]` |
| magnitude floor 0x30 | `+73EBAC` | eax = magnitude 0..255 |
| late sample | `+73E78A` (already used by the mouse fix) | call `SteamInputWork::Update` via `+1596E38` |
| turn truncation (pad) | `+885C5E` / `+885C6F`, camB `+8894A3` / `+8894B8` | already hooked for the mouse carry |

Not hookable safely, or not what 2l.2 assumed: the speed band (it is a state scalar, not a speed),
the drag (dead store), the fast-turn flag (shared with the mouse), `+4FC50` (a shared converter
called from both blocks), and the 60-frame lock-out (its state bits are unknown without a live run).

### The Config Tool needs a Slider kind

The tool has exactly three field kinds (`Field::Type { Bool, Int, Choice }`, `tool/src/fields.hpp:16`),
each mapped to one control in `MainFrame::AddRow` (`tool/src/ui.cpp:483-505`): a checkbox, a
`wxSpinCtrl`, a `wxChoice`. Adding `Slider` means the enum, a factory beside `Field::Int`, and a
case in **eight** branch sites (`fields.cpp:279`, `fields.cpp:296`, `ui.cpp:352`, `489`, `633`,
`660`, `696`, `728`); eleven sites branch on the kind in total, and `settings_io.cpp:16` / `:27`
and `ui.cpp:527` also test it. If `Slider` reuses `defaultInt` / `min` / `max`, the defaults and
validation cases fall through to the existing `Int` ones. `Row::control` is a single `wxWindow*`,
so a slider plus a numeric readout needs either a second `Row` member or a sizer passed to
`grid->Add` (`wxFlexGridSizer::Add` takes a sizer, so no extra panel is needed).

### What to implement, in order

Each step is small enough to judge on its own, and each says what would show it works.

1. **Stick Look Dead Zone** (0 to 48, default 48 = the game). Mid-hook the four dead-zone
   instructions; write the slider's value into xmm0 and rescale the gain at `+73EACB` / `+73EB02`
   to `127 / (127 - dz)` so full deflection still reaches full output. Evidence: a Lab log line of
   the value before and after per frame, and the stick moving the camera at a deflection that does
   nothing today.
2. **Stick Look Sensitivity X and Y** (25 to 400%, default 100). One mid-hook at `+73EB06`,
   pad-only. Evidence: the logged look values scale by the set factor and the mouse's do not.
3. **Stick Response Curve** (50 to 300, default 100 = as shipped). Same hook, applied to the
   magnitude. Evidence: the logged curve of output against deflection.
4. **Stick Late Sample** (off by default). `SteamInputWork::Update` through `+1596E38` at
   `+73E78A`. Evidence: the same start-of-movement measurement as the mouse, 24 ms to about 9 ms.
5. **Stick Fraction Carry** (off by default): extend the existing carry to non-mouse frames.
6. **Stick Move Dead Zone**, once the movement block's gate is understood; it shares the block with
   the keyboard, so it needs the source check.
7. The Config Tool `Slider` kind, so all of the above are sliders rather than spin boxes.

### Risks

- **Shared sites.** The fast-turn flag and `+4FC50` touch the mouse path; they are excluded above.
  At `+73EB06` the pad-or-mouse decision must be re-derived, not read from `player+0x9B2`, because
  that byte is cleared a few instructions earlier.
- **Steam Input sits in front of the game.** Steam's own per-game dead zone, curve and sensitivity
  are applied before the game sees `stick_l` / `stick_r`. Every measurement and tester report must
  state them; the baseline is Steam's dead zone set to none.
- **The options record is saved** to `../mgspw_savedata_win/<steamID64>/usersv` (`+85330` builds the
  path, `+849C0` packs it): the plugin must never write it, and no setting here touches it.
- **A game update moves these RVAs.** Every hook goes in by byte signature with the site's own
  sanity check, as the mouse hooks do.

### Open questions for the user

- Default dead zone: leave at the game's 48 (so an untouched install is unchanged) or ship a lower
  default? Modern games use about 8 to 12 of 127; worn sticks drift, so 0 is not a sane default.
- Should any stick slider change the shipped feel by default, or should all of them default to the
  game's own values?
- Sensitivity as one slider with a separate vertical percentage (as the mouse has), or independent
  X and Y?

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
