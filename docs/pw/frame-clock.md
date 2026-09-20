# Peace Walker: the engine's frame clock, and what raising the tick rate would take

Static analysis of build 25294658, 2026-09-20. Three focused agents, results kept at
`C:\mgspf_tools\pw\mouse\tickrate_recon.json`. Nothing here has been run in the game.

## 1. There is one clock, not a thousand constants

The user's observation that this engine went from 20 fps on PSP to 60 on console, and that sibling
titles have had their rate raised, turned out to be the key. The engine carries a real time base.

**One publisher.** `+7B690`, called exactly once per tick from `+7AE14` (itself registered as a
callback at `+7B512`, not called inline). The frame-period constants — 16.6833 ms, 59.9401 Hz,
0.0166833 s — have **four references in the whole binary, all four inside that one function**.

**One input.** How many 59.94 Hz display intervals this tick consumed: an integer, taken from the
override global `+10A6248` when non-zero, otherwise from `+1084484` via the getter `+77EE0`, plus
any dropped intervals accumulated in `+10A62E4` (then cleared).

**Republished in several flavours**, all written inside `+7B690`:

| global | meaning | reads |
| --- | --- | --- |
| `+E9C238` | the step in 1/299.7 s units, **5 per 60 Hz frame** | 1825, in 1022 functions |
| `+10A5EF8` | the step in frames, float (1.0) | 846 |
| `+E9C23C`, `+10A5F10` | the step in frames, int | 113, 160 |
| `+10A5F08`, `+10A5F0C` | the step in seconds / milliseconds | 11, 1 |
| `+10A5674`, `+E9C224`, `+E9C228`, `+E9C220` | on rate change only: 60/N, 1/N, N, mode id | 209, 14, 467, 19 |

**Consumers subtract the delta rather than stepping by one**, so the engine is already
parameterised: the systems are delta-driven, not fixed-increment.

**The rate mechanism is live code with a working ladder.** `+7B2C0` sets the pair (units, frames):
20/4 = 15 Hz, 15/3 = 20 Hz — the PSP's rate — and 5/1 = 60 Hz. The data ladder `{60, 30, 20, 15}`
sits at `+98F6A0`. This is almost certainly how the 20-to-60 port was expressed, and it is the
mechanism we hoped existed.

**Correction to an earlier note of mine (2m and the frame-pacing note):** I called `+E9C238` a
pooled compiler constant with hundreds of incidental references. That was wrong. It is a store
target at `+7B970` with a previous-frame copy at `+E9C240`, written every tick by the clock and
set to 20/15/5 by the rate setter. It is the most widely read per-frame quantity in the binary,
and camA reads that live global rather than a literal 5. The conclusion I drew from the bad
premise happened to survive; the premise did not.

## 2. The wall is arithmetic, and it is specifically 120

The step is an **integer** number of 1/300 s units. 300 divides evenly by 60, 30, 20 and 15
(5, 10, 15, 20 units) — the whole shipped ladder. **120 fps needs 2.5 units and is not
representable.** Worse, that integer is also used as a *divisor* at 86 sites, and is divided by 5
to produce the frames-per-tick global, so a zero or a rounded value is not harmless.

The rate abstraction is 60/N for integer N, so the engine's own ladder can only go **down**.
Nothing above 60 was ever contemplated.

Two candidate routes, neither tried:

- **Alternate 2 and 3 units per tick**, averaging 2.5 = 120 Hz. Cheap to try, and uneven by
  construction: every other tick advances 20% more than its neighbour, which may read as judder.
- **Rescale the unit base** so that a finer unit exists (120 Hz = 5 units of 1/600 s). This means
  auditing the 86 divide sites and the /5 relationship to the frames global, and anything that
  compares a duration in raw units against a literal.

100 Hz (3 units) and 150 Hz (2 units) are integer-clean and would be far less work than 120.

Also in the way: the pacer `+783C0` blocks until the presented-frame counter `+1084498` has
advanced `+1084484` intervals, **adapts that value and clamps it to a maximum of 3**
(`+785D8`), so it will fight anything a plugin writes there. A tick-rate experiment should ride on
the override `+10A6248` instead. And the engine's tolerance of a zero-frames tick is unknown,
which matters because at a 120 Hz loop the interval count would alternate 0 and 1.

## 3. From the camera angle to the view: the late re-sample point exists

Traced end to end but for the last hop. `+886C40` (chunk `+886D41`) reads pitch `cam+0xA0` at
`+886E20` and yaw `cam+0xA2` at `+886E42`, scales both by 2*pi/65536, builds a pitch-then-yaw
rotation through `+AB090` / `+AB1B0`, swings the boom vector, and writes a finished camera record
on the owner: eye float4 at `owner+0x7D0`, angles as words at `owner+0x7E0` / `+0x7E4`.

**It uses the same tick's angle**, with a structural proof rather than a guess: `+887040` snapshots
the angles into `cam+0xA8` and adds camera shake into `cam+0xA0` immediately before `+886C40`
reads them, restoring the snapshot afterwards. An add-then-undo pair only makes sense with the
view build sitting between this tick's angle integration and the restore.

**So there is a later place to hand the mouse over, and it is the prologue of `+887040`** — after
the camera routine has folded the look deltas into `cam+0xA0/0xA2`, and before the snapshot.
Re-sample there and add the residual as 16-bit words in the same form the shake uses; nothing
downstream needs recomputing. Hooking any later, inside `+886C40` past `+886CFD`, would leave the
scene graph's yaw stale.

Two smoothers sit in that path, at `+887145` (4 frames) and `+886667` (12 frames).

**MEASURED 2026-09-20 and REFUTED: `+887040` does not run in gameplay.** A Lab hook on its
prologue counted **zero calls across 2115 frames** (1777 with the aim camera `camB` active, 339
with the free camera). So whatever that chain serves - a cutscene or vehicle camera, or dead code -
it is not the player's view during play, and the "later re-sample point" it offered does not
exist on the hot path. The traced arithmetic inside it (angles scaled by 2*pi/65536, the boom
swing, the shake add/undo) may still be accurate; it simply is not what runs.

This cost one launch and saved building a feature on a dead path. **What consumes `cam+0xA0` /
`cam+0xA2` for the player's view in gameplay is therefore still unknown**, and a static search for
it is what produced the wrong answer. The way to settle it is the technique that already worked
for the input work: a hardware READ watchpoint on the live camera's angle words for one frame,
logging each distinct accessing instruction. The camera pointer is already in hand at the
truncation hooks.

**Still unknown:** what turns `owner+0x7D0` and `owner+0x7E0/0x7E4` into the view and projection
matrices. A static search is exhausted — nothing reads those offsets directly, so the record is
reached through an interior pointer or copied as part of a larger block. One hardware read
watchpoint on `owner+0x7D0` for a single frame would name it, and we already have that technique
working from the input work.
