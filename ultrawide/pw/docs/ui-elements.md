# Moving UI elements in Peace Walker

How the game draws its UI, what identifies an element, how the patch moves one, the register
of elements measured so far, and where all of this overlaps the MGS4 module. Everything marked
Measured comes from the Lab draw census (`docs/pw/lab.md` in the public tree) on
2026-09-12, at 3440x1440 and 6880x2880 windows, with Afevis's MGSPW-Resolution-Unlocked
loaded (it supplies the widened canvas the fix relies on; see section 6).

## 1. How the UI is drawn (Measured)

Peace Walker is the PSP game inside a native layer. Game logic queues a display list; a native
interpreter replays it and issues DirectX 11 draws on deferred contexts, three command lists a
frame. The UI is drawn straight into the window-sized back buffer, with a full viewport and
scissor, in the same pass as the world. There is no separate UI target and no separate 3D
target: at 21:9 through Afevis's patch the whole frame is drawn at window size.

Every UI draw shares one vertex shader and uploads its own 2640-byte constant buffer before
drawing. Rows 1-4 of that buffer are an orthographic projection of the PSP canvas,
`2/480, 0, 0, 0 | 0, -2/272, 0, 0 | 0, 0, -0.002, 0 | -1, 1, -1, 1`; rows 5-8 are a
**world matrix** whose translation row (floats 28 and 29) places the element in canvas units.
Two kinds of element exist:

- **Quads**: `Draw` of 6 vertices (one quad) or a multiple, from a per-draw vertex upload
  (`Map` with discard on the deferred context, then `Unmap`, then `Draw`). Vertex layout: 24
  bytes textured (`u, v` float; `rgba8`; `x, y, z` float) or 16 bytes untextured (`rgba8`;
  `x, y, z`). Canvas-filling art uses centred coordinates (x -240..240, y -136..136) with a
  world translation of `240 136`; panels and lists use top-left coordinates (0..480, 0..272)
  with a translation of `0 0` or a small offset (`0 30` for the mission list highlight moved
  onto the third row, `147 0` for a column).
- **Text and gauges**: `DrawIndexed` of 4 indices per glyph from a shared vertex buffer that is
  not re-uploaded per draw. Their only per-draw placement is the translation row; vertical
  text carries a 90-degree rotation in rows 5-6 and gauges a 0.75 or 0.70 scale, but the
  translation row is canvas units regardless.

The call order for one draw on its deferred context is
`VSSetShader > PSSetShader > VSSetConstantBuffers(0) > PSSetShaderResources(0) > Unmap cb 2640B > Unmap cb 352B > Unmap vb > Draw`,
so at either `Unmap` the texture and the constant buffer for the coming draw are both known.

## 2. What identifies an element (Measured)

The native call stack is the same for every UI draw (the interpreter's frames), so MGS4's
owner-by-unwind attribution has no purchase here. What does identify an element:

- the **world-matrix translation row** (tx, ty), which is stable per HUD group across frames
  and boots (measured at `(-11,-8)`, `(8,-12)`, ... for the mission HUD, see section 5);
- the **texture** bound to pixel-shader slot 0, by size and format (object addresses change
  per boot; sizes and formats do not);
- for quads, the **canvas rectangle** of the upload (min and max of x, y).

A table entry is therefore (translation row, texture, rectangle), and a HUD group is one
translation row.

## 3. How the patch moves an element (Measured with the live commands)

Two hooks on the deferred context's `Unmap` (installed by replacing the context class's vtable
entries in `d3d11.dll`; an inline trampoline on `Map` crashed the driver):

- **Translation bias** (`tbias`): when the unmapped buffer is a constant buffer whose row 1 is
  the UI ortho, and its translation row matches a registered (tx, ty) within 0.5, add
  (dx, dy) to that row before the game's `Unmap` runs. This moves text, gauges and quads
  alike; it is the handle for the in-game HUD.
- **Vertex bias** (`bias`): when the unmapped buffer is a vertex buffer and the texture bound
  at that moment matches a registered size (and, when given, the decoded rectangle matches),
  add (dx, dy) to every vertex's x, y. This moves one quad without touching others that share
  its translation row (the Mission Selector's title glyph run against its black box).

Both are pure data rewrites of an upload the game already made, keyed on identity, applied
before the draw is recorded into the command list. No game code is patched and nothing depends
on Afevis's hook sites; the shipped fix is the same two rewrites driven by a static register
instead of live commands.

Offsets are **static per side, per aspect**: at 21:9 the canvas Afevis widens is 650 units,
so the edge is 85 units beyond the 480-wide layout on each side; a left-anchored group gets
-85, a right-anchored one +85, centred elements 0. At 4:3 the canvas is 480x360 and the same
rule applies vertically (44 units). The formula is `(canvas - 480) / 2` horizontally and
`(canvas - 272) / 2` vertically, canvas from Afevis's `ComputeScaling` (480 x aspect / 1.7647).

## 4. What the fix must leave alone (Measured)

- **Cutscenes and character cards** render as a 16:9 picture with black bars each side at
  21:9 (the mission intro with Snake and soldiers, the "HUEY" card). They are the game's own
  pillarbox and are not widened.
- **The centre reticle** (four dots at translations `(234..246, 130..142)`) stays centred.
- **The world**: 3D draws have a perspective matrix in rows 1-4, not the ortho, and the
  translation-bias hook ignores them by that test.

And what it must fix beyond the HUD: **canvas-sized fades and overlays** only cover the
central 16:9 (the menu fade-in was captured covering 480x272 with the map grid visible on both
sides). Those are quads at `(-242,-138)-(242,138)` or `(0,0)-(480,272)`; they need widening,
not moving (a vertex rewrite of x0 and x1 to the canvas edges), which the same `Unmap` hook can
do keyed on rectangle.

## 5. The element register (Measured, 21:9, canvas units)

### 5.1 Title screen (30 draws a frame)

| Element | Texture | Rectangle / translation |
| --- | --- | --- |
| backdrop | 512x512 BC1 | (-256,-152)-(256,360), tr 240 131.6 |
| colour-bar strips (4 horizontal, 4 vertical) | 512x512 BC1 | 480x68 bands and 120x272 columns tiling the canvas |
| scan bands (3, untextured) | - | (0,-66)-(240,62) stepping one unit |
| vignette overlays | 256x256 BC1, 256x128 BC3 | (-242,-138)-(242,138) |
| copyright line | 1024x128 BC3 | (-128,116)-(128,132) |
| title art | 2048x1152 BC3 | (-240,-156)-(240,116) |
| "PRESS ? BUTTON" and four other glyph runs | 512x512 BC3 font atlas | y 58..112 |
| button icon | 512x512 BC3 | (-9.9,71)-(3.3,84) |
| noise overlay (1024 quads) | 2048x512 BC3 | whole canvas |

Everything on the title is centred art; at 21:9 it sits centred in the widened canvas and
needs nothing but the overlay widening.

### 5.2 Mission Selector (≈230 draws a frame; top-left origin, tr `0 0` unless noted)

| Element | Texture | Rectangle / translation |
| --- | --- | --- |
| "MISSION SELECTOR" glyphs | 1024x1024 BC3 atlas | (10,7)-(90,24) over a black box (8,7)-(92,24) from 256x128 |
| tab icons (6) | 256x128 BC3 | (5..137, 45..77), 32 apart, each with a 14-unit glyph |
| "L1 R1" | 256x128 BC3 | (126..155, 48..58) |
| column headers | 256x128 BC3 | y 64..78 |
| list rows (4, untextured, stale 256x128 bind) | - | (8,79)-(305,93) stepping 15; highlight = the same quad, tr `0 30` |
| count column, skull column | 256x128, 512x512 BC3 | x 306..325 and 326..351 per row |
| row icons | 256x128 BC3 | (143..157) with tr `147 0` |
| details panel and text | various | below y 138 |

The panel is 480 wide inside the 650 canvas and sits in the middle 16:9 of the window; its
map background is drawn separately (the user reports it offset; not attributed yet).

### 5.3 In-mission HUD (≈285 UI draws in an 8,800-draw frame)

| Group (translation row) | What | Elements | Side (wider) | Side (taller) |
| --- | --- | --- | --- | --- |
| `(-11,-8)` | mission timer, "REMAINING ENEMIES" and count | ~60 DrawIndexed glyphs, 512x512 and 256x256 atlases, rotated | left | top |
| `(8,-12)` | life bar and vertical name text | bars at (8..16, 218..264), 30 glyphs | left | bottom |
| `(-6,38)`, `(-6,32)`, `(-6,6)`, `(-6,7)`, `(-6,12)`, `(-3,6)` | item and weapon column | quads at x 445..472, glyphs | right | bottom |
| `(-5,32)` | item count | one glyph run | right | bottom |
| `(112,4)` | top-right gauge | (460..472, 8..83), scale 0.75 | right | top |
| `(454.8,65.5)` | gauge percentage | 4 glyphs, scale 0.75 | right | top |
| `(133,58)`, `(133,85)` | weapon and item boxes | (435..469, 78..142 / 190..254), scale 0.70 | right | bottom |
| `(234..246, 130..142)` | centre reticle dots | four quads | centre | centre |
| `(8,4)`, `(13.9,9.9)` | enemy alert bar and its value | 256x256 quad at (8..21, 8..84), four glyphs | left | top |
| `(-4,4)` | the weapon-menu star | one 256x128 quad; slides in from `(0,4)` when the menu opens | right | bottom |
| `(-6,0)` | the "N/A" box (and 64 quads drawn always, invisible outside the state) | 512x128 quads | right | bottom |
| `(288..298,121)` | the "N/A" text | three glyphs, two positions | right | bottom |

**Slides.** When the item or weapon menu opens, the right-column groups slide sideways over a
few frames and the register must match every frame of it: the column text from -6 to -17
(rows with ty 38, 32, 6, 7, 12), the weapon box from 133 to 117 (ty 58), the item box from
133 to 113 (ty 85), the star from 0 to -4 (ty 4). The shipped register keys each group on
its ty with a tx range that is exactly the observed slide; wider ranges reached rows the
Options and Mission Prep menus draw at small offsets (2026-09-13).

**Gate.** The register applies only while a mission-only marker row has been uploaded in the
last half second: the timer `(-11,-8)` (not every mission has one), the life bar `(8,-12)`,
the gauge `(112,4)`, the boxes at rest `(133,58)` / `(133,85)`. Title, Mission Prep, Options
and Mother Base never see a move.

**Known, stock:** the weapon menu's white column highlight starts its slide at the canvas
edge, so at 21:9 it begins 85 units further right and lands correctly; the game does the
same at 16:9 on a smaller scale (verified with the canvas off, 2026-09-13). Its quad lives in
the menu panel group `(4.7..0, 0)`, which the register never touches.

The move is half the canvas growth, signed by the side: on a wider canvas of W units, left
groups get dx = -(W - 480) / 2 and right groups +(W - 480) / 2 (85 at 21:9); on a taller
canvas of H units, top groups get dy = -(H - 272) / 2 and bottom groups +(H - 272) / 2 (44 at
4:3, 14 at 16:10). The distance from the screen edge is then the 16:9 one in every case. The
HUD cannot be split along one axis only: the top-right gauge goes right on a wide canvas and
up on a tall one, the life bar goes left and down.

Verified in the mission on 2026-09-12 (21:9, +-85, under Afevis's canvas and again under ours)
and on 2026-09-13 (4:3, +-44, the taller canvas): every group at the edge with the 16:9 margin;
the user's read confirmed both layouts (the 21:9 one after first catching the percentage and
the item count as the two that had been left out).

Not yet tabled: the pause menu, the Codec, alerts and pickups, the Mother Base menus, the
in-mission overlays (damage, colour filter, glare), loading screens.

## 6. What Afevis's mod supplies, and what replaces what

MGSPW-Resolution-Unlocked (MIT) widens the canvas to the output aspect (650x272 at 21:9),
renders the whole frame at the window size, centres the game's 480-wide layout by writing a
half-widening offset into the camera structure, and patches about a dozen edge-anchored
elements one by one at their code sites (colour filter, damage overlay, glare, grenade arc,
hit markers, item markers, movies, loading screens). Its supersampling is an oversized window.

This module keeps the first three (they are the canvas and the centring) and replaces the
per-site fixups with the identity-keyed rewrite of section 3, which reaches every element the
census can table without a byte pattern per element. Whether we ship on top of his ASI or
re-implement the canvas widening ourselves is an open decision; the register does not depend
on it.

## 7. Overlap with the MGS4 module

Shared, and deliberately the same:

- **The methodology.** Per-object identity with static per-side offsets, never positional
  zones or heuristics (the user's rule for MGS4, kept here). A table of elements, one entry
  per identity, each with its own bias per aspect.
- **The workflow.** Lab configuration and marker file, a boot script, a census that logs every
  draw with its identity, live commands to try a bias with the game running, a before/after
  capture the user reads, then the value goes into the register. `bias`/`tbias` are the
  Peace Walker `spread`/`chanbias`.
- **What is left alone.** Cutscenes stay 16:9 (MGS4 draws its own bands; Peace Walker's
  engine pillarboxes them itself), the reticle stays centred, the world fills the window.
- **The edge formula.** Half the widening per side, from the output aspect.

Different, because the engines are:

| | MGS4 | Peace Walker |
| --- | --- | --- |
| UI source | `.la2` layouts, 12.4 fixed-point vertices, per-layer strcodes | PSP display list replayed natively; float vertices in a 480x272 canvas |
| identity | owner attribution by unwinding from the pool-append site to the element's builder | world-matrix translation row + texture + rectangle; the native stack is identical for every draw |
| the hook | the draw-pool append (`+68A933`), one site, all elements | the deferred context's `Unmap` (vtable entry), two uploads per draw |
| the edit | offset the pooled vertices per identity | rewrite the constant upload's translation row, or the vertex upload's positions |
| letterbox | our bands, our fit of the picture, D3D11 and D3D12 | the game's own pillarbox for cutscenes; no bands needed |
| render size | internal buffer raised independently of the window | window-sized rendering only (Afevis's supersampling is a bigger window); a separate internal size is future work |
| overflow risk | 12.4 vertex range at high internal sizes | none for resolution; 16-bit canvas fields only grow with aspect |
| world-pinned labels | node strcode, group-translated | not measured yet (item pickup markers are Afevis's `+FEE61`) |

The public tree says only that the module exists; this file is the description.
