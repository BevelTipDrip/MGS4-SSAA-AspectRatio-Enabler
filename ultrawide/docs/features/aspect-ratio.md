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

So a pure `v8.x` spread moves floating elements correctly but may leave vertex-positioned
widgets where the canvas mapping put them, if their shared anchors are canvas-level rather than
widget-level. **The distinct-`v8` count per frame is now the load-bearing unknown** — a few
canvas anchors would mean the spread needs to act on vertex offsets or per-quad screen centres
instead, with its own tearing hazards; widget-level anchors would keep the two-layer design
intact. The `ANCHOR` sampler that reads `v8` per draw logged nothing in this run despite its
hooks installing (a plumbing gap in the diagnostic, not an answer), so fixing that sampler is
the next tooling task.

## Open questions, in the order worth answering

1. **Distinct `v8` anchors per frame** — a few canvas-level anchors, or one per widget? Fix the
   `ANCHOR` sampler (it logged nothing despite its hooks installing) and count. Decides whether
   the two-layer design above survives contact.
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

### AR-002 — the 3D camera appears to lose vertical FOV at 21:9 rather than gaining horizontal

- **Status:** Active
- **Evidence:** Inferred
- **Affects:** any output wider than 16:9
- **Repro:** as AR-001; compare visible world extent against a 16:9 control of the same scene
- **Evidence detail:** same captures as AR-001. The framing reads as the 16:9 image with top and
  bottom lost, but **vertical squash has not been excluded**, and the user has not separately
  confirmed this reading — hence Inferred, not Observed
- **Screenshots:** same pair as AR-001
- **Popups:** none
- **Cause:** unknown. Consistent with a projection built for 16:9 regardless of output;
  mgs4Ultra120 rewriting `m00` for Hor+ supports that, but that is their finding, not ours
- **Fix:** none. Needs question 3 answered first

---

## Fixed

None recorded.

---

## Retracted / unverified

None recorded.
