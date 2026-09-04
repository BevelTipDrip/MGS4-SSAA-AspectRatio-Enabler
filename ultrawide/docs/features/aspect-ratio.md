# Aspect ratio

**Settings:** none yet — this feature is not implemented
**Implements:** nothing yet. Expected to land in its own `src/fixes/` file rather than inside
`resolution_scaling_fixes.cpp`, which is research and diagnostics

## What it is meant to do

Make the game render and present correctly at aspect ratios it was not authored for — ultrawide
(21:9, 32:9) above all, and 4:3 below.

Nothing here is built. This document exists to hold what is already known before any of it is
written, so the work does not restart from scratch or rediscover facts the UI research already
established.

## Status

**Research started 2026-08-31.** No patch yet. The baseline observation below replaces the old
"no measurement" state, and the reference project's architecture has been studied (none of its
code is used here).

The one existing claim in this area is [WR-001](window-resolution.md#active-bugs) — that a 4:3
window squashes the interface horizontally. It is at **Reported**, from the shipped README, and
has never been reproduced during this project. It stays filed under Window resolution because
that is the setting that exposes it; when this feature fixes it, mark it fixed there with a
pointer back here rather than moving it.

**Update 2026-09-04:** 4:3 is done — both layers run rotated onto the vertical axis
([AR-009](#ar-009--43-output-verified-with-both-layers-rotated-to-the-vertical-axis) below;
mechanism in [ultrawide-hud-identity-spread.md](ultrawide-hud-identity-spread.md#7-the-shift)).
WR-001 in the public fork is to be marked fixed with a pointer only; the public doc must not
describe the mechanism.

---

## What is already known

### The window size is ours to set

`render.windowSizeX/Y` live at `+0x1B00460` and `+0x1B00464` and are already overridden by the
window-resolution patch, independently of the internal render buffer at `+0x1B00000` / `+0x1B00004`.
So arbitrary output dimensions are already reachable — the question is entirely what the game does
with them, not whether we can set them.

### The UI does not derive its coordinates from the aspect

This is the single most relevant finding from the UI research, and the reason 4:3 is expected to
squash. Interface geometry is authored against **fixed virtual coordinate spaces**, observed as:

| Basis | Notes |
| --- | --- |
| 1280x720 | most common |
| 1429x800 | |
| 720x400 | resolution-independent elements |
| buffer pixels | |
| 2 units per pixel | |

None of these are aspect-derived. A wider window therefore stretches the same virtual space rather
than revealing more of it, which is the mechanism a fix has to interrupt.

### Every quad's placement is readable at runtime

Per-draw constants arrive as six `float4`s on slot 1 at stride 0:

| Offset | Name | Role |
| --- | --- | --- |
| — | `v3` | scale |
| — | `v6`, `v7` | basis vectors (rotation) |
| 80 | `v8` | NDC anchor |

with the shader computing `clip = (POSITION x v3) rotated by v6/v7 + v8`, and `o0.w = 1.0` — no
perspective divide. Vertices are 12 bytes: `[int16 u,v][RGBA8 colour][int16 x,y]`, position at
**offset 8**.

That is the whole vocabulary needed to rescale UI horizontally: the anchor says where a quad sits
in NDC, the basis and scale say how big it is, and both are per-draw.

### Where the draws happen

| Path | RVA |
| --- | --- |
| in-game HUD / crosshair | `+7A5DA3` |
| menu | `+7A70BE` |
| vertex upload (Map/memcpy/Unmap) | `+7A8CD0`, called from `+7A583A` |

### The diagnostics already exist

`Log Viewports` and `Watch Staging Writes` in `[Graphics]` dump per-draw shader, call site, vertex
decode, layouts and anchors. **Both are extremely expensive** — with them on the game is
unplayable regardless of GPU — but for a single captured frame they answer most questions about
what a given element is doing. See [tooling.md](tooling.md).

---

## Baseline: what the game does at 21:9 today

Measured 2026-08-31 on the current build with **no aspect patches**: `Window Resolution
(16:9)="3840x1620"` (the parser takes any `WxH` from 320 to 16384; the key's aspect suffix is a
label, not validation), stage `s01a10l_1` via the harness, screenshots reviewed by the user.
Control: the same stage at 3840x2160 from the AA-001 runs.

1. **The HUD stretches to the full output width.** Widgets keep their full-width span while the
   frame is shorter, so everything goes flat; the aiming reticle is visibly an oval. This is
   exactly what the layout converter's native math produces (see below), so it is both observed
   and explained.
2. **The 3D looks Vert-** — the 16:9 horizontal framing with top and bottom lost, not more world
   at the sides. *Provisional: this reading has not been separately confirmed and squash has not
   been fully excluded.* The reference project's need to rewrite projection matrices for Hor+
   supports it.
3. **Presentation quirks in the captures are this machine, not the engine.** The frame sits
   anchored top-left with black fill because the user's GPU is set to no-scaling, and any mode
   smaller than native displays that way. A corrupted block in the bottom-right of one capture
   was a Steam popup glitching, nothing else. Do not read either as engine behavior.

## The layout converter — the key mechanism

The game converts UI layout from logical **1280x720** space to output pixels in one CPU-side
function, at RVA **`+0x439810`** (identified by the mgs4Ultra120 project — see attribution
below; **verified in-process 2026-08-31**: the prologue matches, and the `Log UI Layout` probe
below hooked it live). It takes `(object, x, y, width, height)` in logical units and writes a
mapping into the layout object.

**Measured with the probe** (runs at 3840x2160 and 3840x1620, logs in `C:\mgspf_tools\ar_probe3\`):

- The converter runs **once per layout object, at creation**. Every call in the whole
  boot → menu → stage load → aiming flow — 86 calls, 86 distinct objects, one caller
  (`+435D42`) — passed the full canvas `(0,0 1280x720)`. Per-widget rectangles never go
  through it, and aiming (the reticle appearing) triggered no call at all: widget placement
  happens downstream, in the per-quad constants the GPU research documented.
- What it writes, dumped before/after around the original: a **physical viewport** at
  `obj+0x200..0x20C` — `(0,0,3840,2160)` at 16:9 and `(0,0,3840,1620)` at 21:9 — the
  **logical range** at `obj+0x210..0x21C` — always `(0,0,1280,720)` — and a dirty flag
  `|= 0x220000` at `obj+0x18`. (Field meanings from mgs4Ultra120's reimplementation;
  values measured by us.)

So the model is: each layout object carries an affine map "logical range → physical rect",
widgets position themselves in logical units, and the stretch at non-16:9 is precisely
"0..1280 x 0..720 onto the full output". Two consequences:

- **Fixing shapes is one write per object.** Change the mapping and every widget in the object
  inherits it. mgs4Ultra120's root-canvas trick is to keep the physical viewport full-size and
  *widen the logical range* instead, so logical 0..1280 lands centered while output-covering
  layers can still draw beyond it.
- **An affine per-object map cannot do anchored spread.** Spreading positions more than sizes
  is not affine, so "corners at corners, shapes undistorted" cannot come from the converter
  alone — it needs per-draw anchor adjustment downstream.

This is the same 1280x720 logical space the UI research documented from the GPU side, and the
converter sits **upstream** of the vertex pipeline the reticle truncation fix patches: converter
output feeds the coordinate scaling whose `cvttss2si`/`movsx` narrowing we widened.

**The truncation fix and a layout hook are expected to compose, not conflict.** The fix widens
registers; a layout hook changes the values flowing through them. mgs4Ultra120 ships both
together, which is working evidence. (The removed wrap-around patch *would* have conflicted —
one more reason it stays gone.)

## What mgs4Ultra120 does (studied, not copied)

Their `MGS4NativeCenteredHUD.asi` hooks the layout converter and remaps into a **centered 16:9
safe canvas** — a pure affine squeeze, no per-element anchoring. That is the
centering/stretching the user observed. Native surfaces that bypass the converter get separate
guarded hooks: subtitles, movies, TV movies (a physical-rect emitter at `+0xbe090`
discriminated by caller return address), inventory previews, and two byte-verified fullscreen
modal backgrounds. Their 3D side rewrites `m00` on structurally-identified projection matrices
(Hor+), with an optional FOV multiplier because stock vertical framing reads tight at 21:9, and
cutscene camera rebuild routes handled separately by return address — corroborating that
cutscenes need their own handling.

**Attribution:** the layout converter RVA and the native-surface catalogue above are their
research. If any of it ends up used in our implementation it gets credited in the source and
README, as with the reticle-fix attribution. No code has been taken.

## Where our goal differs

We want the in-game HUD **anchored, not centered**: corner widgets at the real corners of the
output, each element undistorted, floating elements (reticle, markers) keeping their world
positions. Menus may letterbox; the user has said so explicitly.

The design the measurements point to, in two layers:

1. **Shapes, at the converter.** Our own hook on `+0x439810` giving each object the
   expanded-logical-range mapping — full-output viewport, logical 0..1280 landing on a centered
   16:9 area. Every widget everywhere becomes undistorted; menus simply inherit centering,
   which the user has accepted.
2. **Corners, at the draw path.** On the in-game HUD path only (`+7A5DA3`, not the menu path
   `+7A70BE`): scale each draw's `v8.x` NDC anchor by `outputWidth / safeWidth`. Centered-canvas
   edges then land on real screen edges, shapes stay untouched (`v3`/`v6`/`v7` alone carry
   size), and a center anchor — the reticle — maps to itself. For a floating element whose
   position derives from the full-width world, the spread is the inverse of the centering
   compression, i.e. it ends up back where it points.

**The blocking unknown has moved down a level: anchor sharing.** Measured on an aim-time draw
sample (`Log Viewports` + `Watch Staging Writes`, 16000 sampled draws, log in
`C:\mgspf_tools\ar_anchor3\`):

- The UI shader (`c0f6fdb2`) draws through **both** quad paths during gameplay — 3698 draws via
  `+7A70BE` and 255 via `+7A5DA3` in the sample — so `+7A70BE` is not menu-only, and a
  HUD-only patch cannot key on the call site alone.
- **3902 of 3953 UI quads carry their position in the int16 vertex data, offset from their
  anchor** — including symmetric pairs (`mid=(-1680,0)` / `mid=(1664,0)`) that are two halves
  of one widget sharing a centre. Static widgets are vertex-positioned around a shared anchor.
- Floating elements are the opposite: the reticle positions through `v8` — that is the value
  the truncation bug wrapped, which is how we know.

**The anchor census answers it** (sampler repaired — its pipeline-analysis half had been lost
with the removed shader workaround — then 800 aim-time anchors sampled, 0 wrapped, log in
`C:\mgspf_tools\ar_anchor4\`):

- **Anchors are widget-level, and heavily shared.** The ration box is 86 draws on one
  `v8=(-0.719,-0.769)`, the weapon box 53 on `(0.719,-0.769)`, the top-left cluster 123 on
  `(-0.881,0.883)`, the camo meter on `(0.733,0.622)` — all matching the visible HUD layout.
  So the spread does move static widgets: their shared anchor is the widget's, not the canvas's.
- **But satellite anchors exist inside widgets.** The ammo tick row is a run of individual
  anchors at `(0.551..0.736, -0.778)` in ~0.0084 NDC steps, marching across the weapon box.

The satellite finding rules out a *pure linear* `v8.x` spread: it scales the tick row's span by
the spread factor while the box keeps its unstretched width — misalignment inside a single
widget, ~120px at 21:9.

**Retracted: the clustered spread** (delta per draw-order anchor cluster, satellites translated
with their cluster). It was built and it tears widgets, just more subtly than linear scaling.
The user's comparison PDF (2026-09-01) documented four cases from full builds of it: the
reticle's corner dots spread from a square into a rectangle, the weapon silhouette slid out of
its containment box, the compass lost its bullseye with the N drifting off the ring, the ration
icon left its label. The lesson generalises: **any delta that varies with fine anchor position
tears a widget whose parts carry different anchors**, and draw-order proximity cannot guarantee
grouping — parts arrive non-consecutively, and rotated variants sit at odd anchors. Kept here
per the retraction rule; the working design is below.

## Implementation state

**Both layers work at 21:9, user-confirmed 2026-09-01.** `src/fixes/aspect_ratio.cpp`, behind
`Fix UI Aspect` (shapes) and `Fix UI Aspect Corners` (anchoring), both undocumented and off by
default until the remaining items below are cleared. Verified at 3840x1620 by screenshot: corner
widgets at the true screen edges at their 16:9 margins, every widget undistorted, the ammo tick
row aligned inside its box, the reticle round and centred while aiming.

How layer 2 found its real interception point, since two research assumptions fell on the way:

- The two "instance array" sites the earlier UI research recorded (`+7A582B` / `+7A57EA`) carry
  only **tiny constant uploads** — 2 and 3 records per call, forever. (That also retroactively
  explains the old research's own note of "two records of ±2.0".)
- The per-quad pool actually flows through the **generic upload helper at `+7A8CD0`** (Map,
  memcpy, Unmap) from the call at `+7A583A` — with the source pointer as the helper's **fifth
  argument, on the stack**, invisible to any register-based probe. The helper's register shape
  is `(wrapper, resource, destOffset, size)`.
- The pool is ~200KB, double-buffered across two heap allocations, with 96-byte anchor blocks
  at a clean pitch from a non-zero start offset. Blocks are recognised by signature, never
  offset arithmetic, so an unrecognised layout degrades to a no-op.

The spread itself, after one full design retraction (above), is a **constant per-side shift
with a centre dead zone** — deliberately *not* a function of each anchor's position:

- `delta = ±(1 - 1/factor)`, zero inside the dead zone. The constant is exact, not a tuning
  knob: compressed NDC x full width equals safe NDC x safe width, so it preserves each
  widget's **pixel distance from the screen edge** exactly as on 16:9, and every widget
  translates pixel-rigidly. Verified against all four PDF defect cases in full-4K crops,
  user-confirmed 2026-09-01: dots square (104x107 px), silhouette contained, bullseye at the
  arrow tip, icon centred on its label.
- **The dead zone** (inner 35% of the canvas half-width) holds the reticle, its corner dots
  and the stress readout still. The census supports the placement: centre elements sit below
  0.11 of the canvas, corner elements above 0.55.
- **Only canvas-authored anchors shift.** Anchors beyond `|x| = 1/factor` are output-authored
  fullscreen layers (fades, vignettes, pinned at exactly ±1); shifting one slides its coverage
  and uncovers a strip — seen as an orange band over the right sixth of the frame, width
  exactly `(factor-1)/2`, before the exemption.
- **Rotated quads shift with their widgets.** The compass's facing arrow, the diagonal weapon
  silhouette and the objective marker carry rotated bases, which the strict axis-aligned block
  signature rejects; a relaxed signature (v8.z == 1, sane magnitudes, any rotation) is
  consulted only inside an established 96-byte-pitch run, where structural context does the
  discriminating.

## Retracted: the upload-time heuristic spread, in full

A live 21:9 play session (2026-09-01, ~an hour, the user driving) falsified the whole
upload-time approach — the whitelist zones, the world-element decompression, the dead zone and
ramp. **Statically correct, dynamically incoherent**, in the user's words: "everything kind of
floats around the screen... it looks correct statically, but it falls apart in game." Each
class-specific rule fixed its screenshots and made another class worse in motion:

- The expanded radar moved its anchors out of the compass whitelist zone mid-game, so its
  parts fell into the world regime and the objective bullseye drifted off the ring (AR-004).
- World-tracked labels crossed regime boundaries as the camera panned — first jumping at the
  dead-zone threshold, then (after the edge-exemption fix) still subtly inconsistent.
- Every classification was by *where an anchor happens to be this frame*, and elements walk
  across positional boundaries constantly. No refinement of positional rules can converge.

**The requirement the failures establish:** elements must be positioned by **identity**, not by
anchor value, and identity lives in the code that produces each anchor — static widgets and
world-tracked elements are positioned by different producer code paths, and within the static
path each widget's authored logical position is a stable constant. The retained lesson stack:
layer 1 (shapes) is correct and final; rigid translation is the only safe motion for a widget;
world elements need aspect-aware source math, not post-hoc correction.

The heuristic implementation stays in the tree behind `Fix UI Aspect Corners` (off by default,
marked experimental) until the producer-based replacement lands, then gets deleted.

## The LA2 format — the UI's authoring system, from the community

An 010 Editor template for the game's `.la2` UI layout format (author "eol", provided to the
user by a contact, 2026-09-01; local copy in `CypherofX\LA2_MGS_PS3.bt` beside the repo)
connects the runtime measurements to the authoring format:

- **`LA2_PACKET_VERTEX` is `short x, y` in 12.4 fixed-point** — int16 at 1/16-pixel units.
  This is the format whose 2047.94px ceiling caused the reticle truncation bug, and the int16
  positions at offset 8 of the 12-byte runtime vertex are these authored values in flight.
- **LA2 has a Layer table** (strcode + status + object count; `LA_DispOnLayer` /
  `LA_DispOffLayer` script commands), and objects carry `usLayerIndex`. The channel bitmask
  the payload assembler iterates is almost certainly these layers.
- **Identity is authored, by name:** objects, actions, and layers all carry `strcode` hashed
  names, plus object IDs and parent IDs. The endgame injection can key on named elements.
- Authored positions live in the status packet system (`LA_SetXY`, `LA_SetWH`, `LA_SetScale`,
  `LA_SetRotate`, morph animation at a 300fps timebase) — the "static producer" is the
  engine's evaluator for these packets.
- The contact also notes the community mod loader can log every asset the game reads — a
  per-file map of which `.la2` loads for which screen, i.e. file-level identity for free.

## The next arc: producer-based positioning

**The transport chain is now mapped end to end** (write-watch on a live pool slot, then
in-process disassembly, 2026-09-01, logs in `C:\mgspf_tools\ar_prod*.log`):

```
producers -> per-channel record buffers        (channel bitmask at +23BA6710,
                                                descriptor table at +23C74634)
          -> +68A410 assembles the payload      (args (ecx, edx=byte size); builds in its
             in its stack frame                 stack frame; element loop over channel bits)
          -> appends to the 0x32000 pool        (memcpy at +68A933: dst = poolBase+cursor
                                                from the descriptor cluster at +23C949xx,
                                                src = rbp+0x24, size = arg edx; pool
                                                allocated on first use at +68A8C4)
          -> upload helper +7A8CD0 from +7A583A -> staging -> per-frame vertex ring
```

Assorted hard-won specifics: the pool watch fires via `VCRUNTIME140!memcpy <- +68A938
<- +666485 <- +666729 <- +BAFDF <- +BE330`; `+666470` is a thin wrapper whose call at
`+666480` targets `+68A410`; the `+BAFC0` frame reads a global at `+1B063F0`, in the known
render-globals block. Pool slot offsets are **not stable per element** — the pool is an append
stream — which is why upload-time slot identity was never possible.

**The identity mechanism is the game's own channel structure.** The engine keeps UI elements in
separate per-channel streams selected by bitmask; if static HUD and world-tracked overlays ride
different channels, per-channel positioning *is* the comprehensive injection — identity from
game data, no heuristics. Remaining steps:

**Census results** (`Log UI Channels`, three probe placements, logs in
`C:\mgspf_tools\ar_chan*\run.log`): `+68A410`'s arguments are a **vertex range** —
`(startVertex, count)` — not a channel id. A frame submits one bulk batch (660 vertices, the
batched static UI mesh) followed by small 6-vertex batches, one quad each: the per-draw-anchor
elements, matching the aim-time draw taxonomy. Its internal loop at `+68A9C8` emits per-GPU-
stream chunks (slots 1/2/4/5, strides 0xC vertex / 0x10 float4). So `+68A410` is the
**draw-batch builder**, one level below identity: element names live above it, in the la2
object/layer evaluation that fills the vertex arrays and the `+23C7xxxx` tables it reads.

**The la2 census ran** (`Log LA2 Loads` — our own hook on the game's archive read, byte
signature from cipherxof's MGS4-ModLoader (MIT), header parsing per the 010 template; log in
`C:\mgspf_tools\ar_la2\run.log`): **176 distinct `.la2` files** in one boot-to-aim, all
version 1.9 **big-endian** (the PS3 data verbatim — the template applies unchanged), each
header parsed live with its layer table. Most files carry **4 objects — single-widget files**
— with a few large composites (117 and 42 objects, the main HUD assemblies). **File identity
is approximately widget identity.**

Remaining steps, converged:

1. **Map layout objects to la2 files.** Both events happen at load and both are hooked — the
   archive read (path + buffer) and the converter (`+439810`, object pointer, one call per
   object at creation). Correlate by order/timing, or trace the buffer pointer into object
   creation. Result: every runtime layout object labeled with its file strcode.
2. **Per-object placement in the converter hook** — bias each object's logical range by its
   file's assigned treatment: corner-shift for widget files, centered for menus, untouched
   (or aspect-corrected projection) for the world-tracking files. Rigid whole-widget
   translation, no per-quad classification anywhere.
3. **Delete the upload-time spread** once the above works.

## The working design: identity-keyed draw-time spread (2026-09-02/03)

The zone-based pool spread below was replaced by an **identity-keyed** version: each record is
attributed at the pool append by a stack unwind to its owner object, channel index and (for the
selector overlay channel) its script element offset, and shifted by the constant per-side
amount only if that identity is in a measured table. Verified live by the user 2026-09-03
with both wheels open and the HUD closed. The full account - transport chain, the unwind, the
channel and element maps, the lab method, the dead ends - is in
[ultrawide-hud-identity-spread.md](ultrawide-hud-identity-spread.md). The sections that
follow are the history that led there and are kept as the record.

## Identification pipeline and current anchor state

The corner anchoring now lives in the layout converter as a per-la2-strcode table (rigid
whole-object translation; identity read from `obj+0x1C`). **Pinned and verified: the life bar
(`00b66172`, its la2 says "OLD SNAKE") and the camo compass (`003e6492`, "CAMO %").** The
weapon box and ration box await identification: the weapon box's labels live in the 562-object
compound `0059ffd5` whose visible box moved under none of ten occurrence biases (needs
draw-time attribution), and the ration box's file is unnamed.

The identification tooling, built at the user's direction to replace bias-run archaeology:
`Dump LA2` writes every la2 and txn at load; `C:\mgspf_tools\la2_preview.py` renders contact
sheets (all files, or `--runtime` for the ~30 that get layout objects) with authored color
fills and each file's own strings; `C:\mgspf_tools\txn_decode.py` converts txn textures to PNG
(format per the MIT TxnFile.cs in otac0n's Codec, reimplemented with attribution). Gap: most
txn pixel data lives in external `<fileid>_<n>.data` files that bypass ArchiveRead — they need
a SlotResourceLoad hook (signature available from cipherxof's mod loader) before the sheets
can show real widget artwork.

## Remaining work, in the order worth doing

1. ~~**Menus and cutscenes at 21:9**~~ — done 2026-09-04 (AR-010, [letterbox.md](letterbox.md)):
   cutscenes, codec, pause menu and title are kept at 16:9 with bands, gated structurally.
   Submenus and the D1 video are still unchecked.
2. **The 3D camera in gameplay** (AR-002) — the world is Vert- at 21:9 (confirmed by the
   cutscene measurement: m00 follows the window, m11 is the 16:9 value). Cutscenes are now
   zoomed out to the 16:9 frame; gameplay is deliberately left Hor- pending a decision on
   Hor+ (the gameplay route `+0x0BA3A3` is the same builder).
3. ~~**4:3**~~ — done 2026-09-04 (AR-009). Left to check there: the 3D view (cropped or
   squashed?). The BASIC CONTROLS panel stopping at the 16:9 band is by design — it is a menu
   (user, 2026-09-04).
4. **A real play session at 21:9** — the harness sees one stage and one aim; an hour of play
   sees codec, item switching, alerts, and everything else.
5. **32:9** — nothing in the math is 21:9-specific, but factor 2.37 stretches the cluster
   epsilon's assumptions; verify before claiming it.
2. **Confirm or exclude squash vs Vert- for the 3D** (baseline finding 2), then find where the
   projection is built and whether window dimensions reach it.
3. **Does WR-001 reproduce?** Set 4:3, capture the HUD, compare against 16:9. The measured
   converter mapping predicts a horizontal squash; promoting WR-001 to Observed would confirm
   mechanism and bug in one run.
4. **Where do non-full-canvas converter calls happen?** mgs4Ultra120 handles them, so they
   exist — but 86 of 86 observed calls were full-canvas. Codec and letterboxed sequences are
   the likely sources; find one before assuming the hook's non-root branch is dead code.
5. **Do cutscenes differ from gameplay?** Expected yes (separate camera rebuild routes).
6. **What does the menu do at the new mapping?** Draws through `+7A70BE`; letterboxing is
   acceptable per the user, but it must not break.

## Design constraints to respect

- **The correct fix for the UI is almost certainly to leave it at 16:9 and centre it**, not to
  stretch it to fill. Stretching is what is already wrong at 4:3.
- **Do not reuse the wrap-around approach.** An earlier UI workaround that manipulated coordinates
  wholesale is what caused the broken menu text the user saw
  ([SS-003](supersampling.md)), and it has been removed. Per-quad correction with a known anchor is
  a different and safer proposition.
- **Keep it out of `resolution_scaling_fixes.cpp`.** That file is research. Shipping patches live
  in `graphics_settings.cpp` or a new sibling, per the split already made.

---

## Active bugs

### AR-001 — the HUD stretches to the full output width at non-16:9 output

- **Status:** Active
- **Evidence:** Observed
- **Affects:** any output aspect other than 16:9. Observed at 3840x1620 (21:9); the same
  mechanism predicts the 4:3 squash described by WR-001
- **Repro:** `Window Aspect Ratio="16:9"`, `Window Resolution (16:9)="3840x1620"`, load any
  stage, look at the HUD; aim to see the reticle become an oval
- **Evidence detail:** harness run 2026-08-31, stage `s01a10l_1`, screenshots reviewed by the
  user. Widgets hold their full-width span while the frame shortens; reticle visibly oval
- **Screenshots:** [`AR-001-hud-stretched-3840x1620.jpg`](evidence/AR-001-hud-stretched-3840x1620.jpg)
  vs control [`AR-001-hud-control-3840x2160.jpg`](evidence/AR-001-hud-control-3840x2160.jpg).
  Ignore the top-left anchoring, bottom black fill and the corrupted block bottom-right — the
  first two are the user's GPU no-scaling setting, the last a glitched Steam popup
- **Popups:** none
- **Cause:** the layout converter's linear map (`x * outputWidth / 1280`) assumes the output is
  the same aspect as its 1280x720 logical space. Mechanism section above
- **Fix:** none yet — this bug *is* the feature

### AR-005 — runtime anchoring drags menu and selector copies of the same content

- **Status:** Active (mechanism confirmed); the runtime anchoring is disabled by default
  pending a fix
- **Evidence:** Observed — eight user screenshots, 2026-09-01, in
  `C:\mgspf_tools\Live Playtest bugs\`
- **Affects:** the lab switch `Aspect Runtime Anchoring=1` (with both aspect keys on)
- **Repro:** enable it at 21:9, open the Weapons menu or the quick selector
- **Evidence detail:** in the user's words — in the weapons menu "the AK icon (and some
  graphical elements associated with it) are offset to the right" while its row text stays
  put; in the quick selector "you can see the AK is offset, along with that brighter
  transparent box, but every other element of the weapon widget is in the original 16:9
  location"; a different selected weapon "is not offset, but other elements in that area
  are"; "the item widget behaves in the same way the weapon widget does"; the same glitch
  "applies to the rations in the items menu". Confirmed correct in the same session: both
  top corners, and the weapon box itself at the bottom-right in gameplay
- **Popups:** none (but see the crash records below)
- **Cause:** the runtime rule anchors *every instance* of the weapon/ration content
  strcodes (00CD1BC3 / 002506A9). The same content renders in the full menu, the quick
  selector, and the selector highlight; those copies get dragged to the HUD corner. The
  strcode names the asset, not the context
- **Fix:** solved 2026-09-01, at conversion time rather than runtime. Per-instance live
  bias probes (lab session, `bias <index> 0 -300` on every registered instance) showed
  the panel files are instantiated ten times at once and only the *first-created* pair
  moves the visible HUD panels; the nine later pairs and every later content copy back
  the selectors and menus. The corner table now carries a `firstInstance` flag: anchor
  the first live object of the strcode, leave every later copy centered. Earlier
  instances are identity-checked (`KnownObjectStillValid`) rather than counted, so a
  stage reload's dead entries cannot veto the new HUD copy. Verified at 3840x1620:
  boot log shows one anchor + nine declines per panel file, gameplay capture has all
  four corners pinned, and a weapon-selector capture in the same boot shows the
  selector untouched. The 00DE6517 all-occurrence entry (whose ten anchored copies
  were themselves dragging selector slots) is dropped: the whole bottom-right widget
  measurably moves with 0059FFD5 + 00CD1BC3 alone.
  **Amended same day, after testing on a real save (Act 4, full loadout): only the
  panel half of this fix survives.** 00CD1BC3 and 002506A9 are not generic "weapon/
  ration content" files - every weapon and item has its own la2, created lazily on
  first display (a dozen new strcodes appeared in the registry the first time the item
  wheel opened), and those two are merely the AK-102's and the ration's, which happened
  to be the loadout on the test stage. On the real save the 002506A9 entry anchored the
  RATION label at its wheel-born first conversion and dragged it onto the CIGS slot
  (AR-008). The content entries must come out of the table; context discrimination for
  box content is unsolved

### AR-006 — the runtime anchoring build crashed twice in play

- **Status:** Active; mitigated by disabling the runtime anchoring by default
- **Evidence:** Measured — the game's own dumps, `mgs4_crash_260901_191512` and
  `mgs4_crash_260901_192459`
- **Evidence detail:** both EXCEPTION_ACCESS_VIOLATION; the first in engine code
  (consistent with a stale-pointer heap write from the re-apply thread — VirtualQuery
  passes on a freed-and-reused page), the second unwinding through VCRUNTIME140 *after*
  identity validation (strcode + engine pointers at +0x20/+0x28) was added, so the
  hardened check is not sufficient either
- **Cause:** writing engine-owned heap objects from a foreign thread on a timer is
  fundamentally racy with the engine's own lifetime management
- **Fix:** none at this approach's level. The eventual fix likely moves the write into an
  engine-driven callsite (the converter re-run, or whatever rewrites these objects'
  mappings post-creation) instead of an asynchronous thread. Moot for the corner feature
  since the AR-005 fix landed at conversion time — on the converter's own thread, inside
  the hook — so the runtime path stays a lab switch with no shipping role

### AR-007 — a death/continue reload leaves the bottom boxes unanchored

- **Status:** Active
- **Evidence:** Measured — anchor log 2026-09-01 20:31:21 plus a post-respawn capture
  (`respawn_poll.png`): tops anchored, Solid Eye box at the centered position
- **Affects:** `Fix UI Aspect Corners=1`, after Mission Failed -> Continue
- **Repro:** die, Continue; the reconverted panel pair logs "later instance left centered"
- **Cause:** the pre-death pair still passes `KnownObjectStillValid` when the new pair
  converts (its page intact, strcode and engine pointers unchanged), so the dead first
  instance vetoes the live one. Identity-checking earlier instances is not enough across
  a reload where the old heap is not yet reused
- **Fix:** none yet. Needs real lifetime tracking — hooking object destruction, or
  invalidating the registry on stage transition

### AR-008 — the selector wheel is torn by corner anchoring; content is per-weapon files

- **Status:** Active — this is the corner feature's real remaining problem
- **Evidence:** Measured + Observed, lab session on the user's Act 4 save, 2026-09-01
  (`fixon_wpn.png`, `fixon_itm.png`, unfixed references `ref_wpn_*.png`/`ref_itm_*.png`)
- **Evidence detail, in order of discovery:**
  - the quick-select wheel is a rigid cross that *grows out of the corner box*: the
    selected slot IS the HUD weapon/ration box (same panel pair). On 16:9 they coincide;
    with the box anchored at the true corner the cross stays centered and the selected
    slot tears out of it
  - every weapon and item has its own content la2 (icon+name+count), created lazily at
    first display; a dozen new strcodes appeared the first time the item wheel opened.
    "00CD1BC3 = weapon content, 002506A9 = ration content" was a loadout artifact
  - the wheel's slot backings and icons follow **no** la2 logical range: biasing every
    multi-instance strcode in the registry (14 files, ±213 and vertical) moved labels
    like NONE/RATION but never the cross chrome — the wheel positions its quads
    programmatically at draw time (the per-quad NDC anchor path in the transport map)
  - wheel objects are created on *first* open and persist across opens (one conversion
    burst across five opens), so conversion-time logic cannot track open/close state
- **Cause:** conversion-time anchoring is static; the box's correct position is dynamic
  in *two* independent ways — corner in gameplay vs wheel-junction while selecting, AND
  the content is the currently-equipped weapon/item's own la2, which changes as you
  cycle. Both the panel and its content have to move together and only in gameplay.
- **Control-point search (2026-09-01, barrel save), all negative so far:**
  - *engine per-state anchor* — a memory diff of the descriptor region across closed/
    weapon-open/item-open states (tools `memsnap.ps1`/`snapdiff.ps1`) found exactly one
    state-correlated float pair at `+23C74930`: NDC `(-0.539,-0.911)` closed →
    `(0.270,-0.769)` on weapon-wheel-open, item-wheel leaves it. But writing it with
    `memwrite.ps1` moved nothing — the engine recomputes it every frame, so it is a
    **state indicator, not a control input**; and its closed value is bottom-*left*, so
    it tracks the selection cursor, not the bottom-right box.
  - *rect emitter* (`rectlog`) — carries only fullscreen scissor/viewport rects
    (`0,0 3840x1620` and the 2× internal-res `7680x3240`), no widget rects. Not the
    box chrome.
- **Fix: design (a), built and verified live the same night — the wheel-aware draw-time
  spread (`Aspect Pool Spread=1` with `Fix UI Aspect Corners=0`).** The pooldump
  experiment measured the wheel's own signature in the instance pool: a column of slot
  anchors at exactly the box x (|normX| = 0.719) above the box band and a row at the box
  y (-0.769) stepping across at pitch 0.3595, present only while the wheel is held; the
  box's cluster (ammo ticks at -0.7785) present only while it is closed. `SpreadPool`
  now reads that state per frame (`DetectPoolState`) and moves an open wheel's whole
  cross with its box, so the wheel arrives at the corner rigid — verified at 3840x1620
  on the Act 4 save: weapon wheel rigid at the right corner (`ps3_wpn.png`), item wheel
  rigid at the left (`ps2_itm.png`, after widening the bands: a full-inventory row runs
  from the junction past centre to the far side, safe to claim wholesale because the
  game hides the opposite box while a wheel is up), pause menu whole and centered
  (`ps3_menu.png`, via the near-edge menu signature standing the spread down), and the
  gameplay HUD's four corners pinned (`ps2_closed.png`).
  What the draw-time form solves by construction, that conversion-time could not:
  - **equipped content** — the pool carries whichever weapon/item is equipped at the box
    anchor, so the zone shift moves panel and content wholesale; no per-weapon file
    table exists at this level
  - **AR-007** — stateless per frame, nothing to veto across a reload
  - **world decompression** — the Solid Eye ramp rides along again
  - possibly **AR-004** — the expanded radar captured at the corner with the bullseye on
    the ring; needs the user's eyes
  All four captures pending the user's confirmation, per protocol.

### AR-004 — the objective bullseye leaves the radar in expanded mode

- **Status:** Active
- **Evidence:** Observed
- **Affects:** `Fix UI Aspect Corners=1` (the retracted heuristic build) with the radar expanded
- **Repro:** 21:9, corners enabled, expand the Solid Eye radar, watch the objective bullseye
- **Evidence detail:** user screenshot 2026-09-01 — radar enlarged mid-screen, bullseye
  floating well off the ring; in the user's words, "bullseye leaving the compass, going
  wherever it likes"
- **Screenshots:** user-provided in session; not yet in evidence/
- **Popups:** none
- **Cause:** the whitelist zones assume the compass sits small in the top-right. Expanded, its
  anchors leave the zone and fall into the world-element regime, where parts with different
  anchors scale by different amounts. An instance of the general failure that retired the
  heuristic (see the retraction above)
- **Fix:** subsumed by the producer-based rework; not worth patching in the heuristic

### AR-002 — the 3D camera appears to lose vertical FOV at 21:9 rather than gaining horizontal

- **Status:** Active for gameplay; fixed for cutscenes by AR-010
- **Evidence:** Observed, 2026-09-04 — projection matrices logged at the setter `+0x0E3410`
- **Affects:** any output wider than 16:9
- **Repro:** as AR-001; compare visible world extent against a 16:9 control of the same scene
- **Evidence detail:** at 3840x1620 the stock projection has m00 = 2.41421 and m11 = 4.29193;
  m11 is the 16:9 value, m11/m00 = 2.3704 = the window's ratio. So the vertical field is the
  16:9 one and the horizontal field is *narrower* than 16:9 would give at that width — Vert-,
  no squash. The user's earlier read of the cutscene close-up (equal width, top and bottom
  lost) agrees
- **Screenshots:** same pair as AR-001; [AR-010 cutscene sheet](evidence/AR-010-cutscene-21-9.jpg)
- **Popups:** none
- **Cause:** the camera builder `+0x0B9BB0` derives the projection from a scale that does not
  know the window; the aspect comes in separately
- **Fix:** cutscenes: the builder's scale divided by the spread factor on the cinematic
  routes (AR-010). Gameplay: none yet — the same builder via `+0x0BA3A3`, the same division
  would give Hor+, but whether gameplay should be Hor+ or stay as authored is the user's call

---

## Fixed

### AR-003 — the corner spread tore widgets apart internally

- **Status:** Fixed, 2026-09-01 (pre-release; never shipped)
- **Evidence:** Observed — user comparison PDF against `ar_fix1` (layer 1 only) and `ar_fix13`
  (clustered spread), then full-4K crops of the fix, user-confirmed
- **Affects:** development builds with `Fix UI Aspect Corners=1` before the constant-shift
  rewrite
- **Evidence detail:** four documented cases, in the user's words: the crosshair's four corner
  dots "in a rectangular pattern relative to each other" instead of square; the gun icon
  "moving out of the containment area"; the compass with "the red bullseye missing, the
  compass arrow not centered in the larger circle, the N indicator offset"; the ration icon
  "not centered over the word ration"
- **Screenshots:** fixed state: [compass](evidence/AR-003-compass-fixed.jpg) /
  [crosshair](evidence/AR-003-crosshair-square-fixed.jpg) /
  [weapon](evidence/AR-003-weapon-contained-fixed.jpg) /
  [ration](evidence/AR-003-ration-centered-fixed.jpg)
- **Popups:** none
- **Cause:** the spread delta was a function of each anchor's position (scaled per draw-order
  cluster). Widget parts carry different anchors — satellites, rotated variants,
  non-consecutive draws — so parts moved by different amounts. Any position-dependent delta
  has this failure; grouping heuristics only change where it shows
- **Fix:** constant per-side shift with a centre dead zone (`delta = ±(1 - 1/factor)`), which
  translates every widget pixel-rigidly and preserves 16:9 edge margins exactly. Design
  section above
- **Verified:** all four cases re-checked in full-4K crops of the same stage; confirmed by the
  user 2026-09-01

### AR-009 — 4:3 output, verified with both layers rotated to the vertical axis

- **Status:** Fixed, 2026-09-04 (the WR-001 case)
- **Evidence:** Observed — lab captures at 2880x2160, read by the user
- **Affects:** `Window Aspect Ratio = 4:3` (any of its resolutions), Centered or Expanded HUD
- **Evidence detail:** with the spread off, the interface is a 16:9 canvas centred in the frame
  (layer 1 alone: shapes correct, widgets 270 px inside the top and bottom edges). With the spread on, the weapon and item panels sit on the bottom edge, the life bar
  and camo ring on the top edge, and each wheel hangs from its box with the dim backdrop
  covering the full height. Log: `logical range 0..1280 x -120..840`, five widgets
  `anchored top|bottom by 0.2500`, backdrops `stretched by 1.3333`. User: "4:3 appears to be
  working" (HUD), "these look correct" (wheels)
- **Screenshots:** [HUD, spread on](evidence/AR-009-hud-4-3-2880x2160.jpg) /
  [HUD, spread off](evidence/AR-009-hud-4-3-spread-off.jpg) /
  [weapon wheel](evidence/AR-009-weapon-wheel-4-3.jpg) /
  [item wheel](evidence/AR-009-item-wheel-4-3.jpg)
- **Popups:** none
- **Cause:** same as AR-001, on the other axis — the converter maps 720 logical rows onto the
  full output height
- **Fix:** `VerticalSpreadFactor()` / `SideShiftY()` in `aspect_ratio.cpp`; every identity
  carries `side` and `sideY`, and exactly one axis is active for a given window. Section 7 of
  the identity-spread doc
- **Verified:** one boot, one loadout (Act 4 barrel save), one resolution; the 5:4 entries
  (factor 1.4) have not been booted but use the same rule

### AR-010 — cutscenes, codec, pause menu and title are letterboxed to 16:9

- **Status:** Working in the lab, 2026-09-04 (`Aspect Letterbox=1`, lab settings only)
- **Evidence:** Observed — 21:9 cutscene framing confirmed by the user; the load path at
  21:9 and 4:3 confirmed by the user (2026-09-04, watched live); the menu captures are the
  lab's read
- **Affects:** any output that is not 16:9
- **Evidence detail:** [letterbox.md](letterbox.md). Cutscene at 3840x1620: camera scale
  divided by 1.333 on the cinematic routes only (m00 2.41421 → 1.81066, m11 held), bands at
  the sides. Pause menu, codec, title and main menu: bands whenever one of five tabled
  layout elements is appended to the draw pool; gameplay between them shows none
  (`boxstat`: `screen-live no`). 4:3: no zoom, bands 270 px top and bottom. Boot phase:
  bands from launch to the key press on the loaded-save plate (the plate's show nodes and
  the dismissal's two channels are one-shot HUD-owner identities), then a post-load grace
  mutes the cutscene signal over the level's intro pan so the bands drop at the press
- **Screenshots:** [cutscene 21:9](evidence/AR-010-cutscene-21-9.jpg) /
  [cutscene 4:3](evidence/AR-010-cutscene-4-3.jpg) /
  [pause & codec 21:9](evidence/AR-010-pause-codec-21-9.jpg) /
  [title 21:9](evidence/AR-010-title-21-9.jpg) / [pause 4:3](evidence/AR-010-pause-4-3.jpg) /
  [load path 21:9](evidence/AR-010-load-21-9.jpg) / [load path 4:3](evidence/AR-010-load-4-3.jpg)
- **Popups:** none
- **Cause:** the 3D projection is Vert- (AR-002) and the menus are 16:9 canvases drawn into a
  wider window with the world showing past them
- **Fix:** builder-scale division under the cinematic owner's scope + per-Present black
  rects, gated on the cinematic camera tick or a tabled screen element within 150 ms, or
  the boot-phase ladder (Armed → PlateUp → Over) with camera-alone fallbacks
- **Verified:** one stage's opening cutscene, the barrel save's pause menu and codec, the
  title and main menu, the LOAD GAME path into the barrel save; 3840x1620 and 2880x2160.
  Submenus, the D1 video, the codec at 4:3, NEW GAME and other saves are not

### AR-011 — the loading spinner stretches with the window and sits outside the 16:9 frame

- **Status:** Fixed in the lab, 2026-09-04 (under `Aspect Pool Spread`)
- **Evidence:** Observed — user ("it's stretched ... obviously its own object we are not
  forcing to conform to the 16:9 space"); fix confirmed by the user on the load-path sheets
- **Affects:** the top-right spinner on the boot and save-load plates, any non-16:9 window
- **Evidence detail:** [letterbox.md §2.2](letterbox.md). Its record carries the rect as
  window fractions (x 0.9068, y 0.8815, w 0.05, h 0.0889), so it is a 183x132 px oval in
  the 21:9 corner at 3840x1620
- **Screenshots:** [load path 21:9](evidence/AR-010-load-21-9.jpg) /
  [load path 4:3](evidence/AR-010-load-4-3.jpg)
- **Cause:** a 48-byte record appended from `+B5830` / `+B58E7` by the static object
  `module+0x23CF7E88`, not a layout node - the identity spread never saw it
- **Fix:** `LoadIconAppend` keys on that object (frame-1 rbp 0, rsi = the object) and maps
  the rect into the centred 16:9 frame on the spread axis (x' = (1 − 1/f)/2 + x/f, w' = w/f)
- **Verified:** 21:9 → (0.8051, 0.8815, 0.0375x0.0889); 4:3 → (0.9068, 0.7861, 0.05x0.0667)

---

## Retracted / unverified

None recorded.
