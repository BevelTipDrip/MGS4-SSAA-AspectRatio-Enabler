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

**Not started.** No patch, no setting, no measurement of how the game behaves off 16:9.

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

## Open questions, in the order worth answering

Everything below is unmeasured. The first three are cheap and should come before any patching,
because they determine whether this is a UI problem, a camera problem, or both.

1. **What does the game actually do at 21:9 right now?** Set an ultrawide window resolution and
   look. Is it Hor+ (more visible at the sides), Vert- (the 16:9 image cropped top and bottom), or
   stretched? This single observation decides most of the design.
2. **Does the projection matrix respond to the window size at all**, or is the FOV fixed? Find
   where the projection is built and whether window dimensions reach it.
3. **Does WR-001 reproduce?** Set 4:3, capture the HUD, compare against 16:9. It is at Reported
   and needs promoting to Observed or retracting before anything is built on it.
4. **Are the black bars, if any, drawn or letterboxed by the swapchain?** Different fix entirely.
5. **Do cutscenes differ from gameplay?** They frequently use their own camera and framing, and
   are the usual place an aspect fix breaks.
6. **What does the menu do?** It draws through a different path (`+7A70BE`) and may well need
   separate handling from the in-game HUD.

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

None recorded — nothing is implemented yet.

---

## Fixed

None recorded.

---

## Retracted / unverified

None recorded.
