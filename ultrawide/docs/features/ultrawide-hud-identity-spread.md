# The ultrawide HUD fix: how it works and how it was made

**Settings:** `Fix UI Aspect=1` + `Aspect Pool Spread=1` (with `Fix UI Aspect Corners=0`), both
in `[Graphics]`, both lab-only and off by default at the time of writing
**Implements:** `src/fixes/aspect_ratio.cpp`
**State:** research build, verified live at 3840x1620 by the user on 2026-09-03 ("This looks
great"). Not shipped. The umbrella document is [aspect-ratio.md](aspect-ratio.md); this one is
the full account of the mechanism that finally worked, written so that someone else can
reproduce it, extend it to widgets not yet covered, or port the idea to another engine.

---

## 1. What it does, in one paragraph

At any output wider than 16:9, MGS4 stretches its whole interface to fill the frame. This fix
makes every UI element the shape it is on 16:9, keeps the gameplay HUD's corner widgets at the
real corners of the wide screen (with the same pixel margins they have on 16:9), keeps the
weapon and item selector wheels attached to their corner boxes when you open them, and leaves
everything else - menus, subtitles, the reticle, the CAUTION banner, anything that tracks the
3D world - exactly centred where a plain 16:9 canvas would put it. Nothing is stretched, nothing
is guessed from where it happens to be on screen, and on a 16:9 output the code is a strict
no-op.

## 2. The problem

The game authors its interface in a fixed **1280x720 logical space** and converts it to output
pixels with one function, the layout converter at RVA `+0x439810` (identified by the
mgs4Ultra120 project, verified and measured here). It runs once per layout object at creation,
always with the full canvas `(0,0,1280,720)`, and writes into the object a *physical viewport*
(`obj+0x200..0x20C`, the full output size) and a *logical range* (`obj+0x210..0x21C`, always
`0..1280 x 0..720`). That mapping is an affine stretch of 1280x720 onto whatever the output is.
At 3840x1620 that is a 4:3 horizontal stretch of every widget: the reticle becomes an oval, the
life bar goes long.

Two things have to happen to fix it, and they turn out to need two different interception points:

1. **Shapes** - every widget must be drawn at its 16:9 proportions. An affine map can do this
   (map the logical space onto a centred 16:9 area instead of the full width).
2. **Positions** - the gameplay HUD's corners have to move outward to the real screen corners,
   *without* the widgets deforming and *without* moving the things that must stay centred. An
   affine per-object map cannot do this: "positions spread more than sizes" is not affine.

## 3. Layer 1 - shapes, at the layout converter

`ConvertLayout` in `aspect_ratio.cpp` inline-hooks `+0x439810`, lets the original run, then
rewrites the logical range it wrote. Rather than shrinking the physical viewport (which would
clip full-frame overlays such as fades and letterbox bars, which legitimately cover the whole
output), the **logical range is widened**:

```
safeWidth = outputHeight * 16 / 9              // 2880 at 3840x1620
extraX    = (outputWidth - safeWidth) * 640 / safeWidth   // 213 logical units at 21:9
logical   = (-extraX, 0) .. (1280 + extraX, 720)
```

Logical `0..1280` now lands on a centred 2880-pixel area. Every widget in every object is
undistorted and centred. Narrower-than-16:9 outputs get the same correction rotated (`extraY`).
Menus stay like this permanently, which is what the user asked for: "menus may letterbox".

This layer is final and has been stable since 2026-09-01. Everything below is about layer 2.

## 4. Layer 2 - positions: why the obvious approaches were wrong

The corner spread was rebuilt four times. The failures are recorded because they define the
requirement the final design satisfies.

| Attempt | Idea | Why it failed |
| --- | --- | --- |
| Linear anchor scale | multiply every quad's NDC x anchor by the spread factor | parts of one widget carry different anchors (ammo ticks march across the weapon box at their own anchors); scaling them by different amounts tears the widget (AR-003) |
| Clustered scale | group anchors by draw-order proximity, translate each cluster | the same tear, subtler: reticle dots became a rectangle, the weapon silhouette slid out of its box, the compass lost its bullseye (AR-003, user PDF) |
| Constant shift + positional zones | shift by a constant per side, decide *what* to shift by where its anchor sits (dead zone, corner zones, wheel bands, menu signature) | "statically correct, dynamically incoherent" (user, after an hour of play): elements walk across positional boundaries constantly - the expanded radar, panning world labels, stale menu records - and every screen state needed another rule |
| Conversion-time per-la2 table | translate whole layout objects by their la2 file strcode | the selector wheel is drawn programmatically at draw time and does not follow any object's logical range; content files are per-weapon and instantiated ten times (AR-005, AR-008) |

Two lessons survived all four and are now design rules:

- **Rigid translation by a constant is the only safe motion for a widget.** Any delta that
  varies with position tears something.
- **What to move must be decided by identity, never by position.** In the user's words: "I
  want to know exactly what each object is, exactly what its anchor is, and I want to move
  them all equally." Outside gameplay and weapon/item switching, no offset at all - just the
  centred canvas from layer 1.

## 5. The draw transport - where the anchors live

Every UI quad reaches the GPU with a 96-byte constant block ("anchor block") that positions it:

| Offset | Field | Meaning |
| --- | --- | --- |
| +48 | `v6` | x basis: `(2/W, 0)` for an unrotated quad; `v6.x = 0.001172 = 2/1706.67` at 21:9, i.e. element-local units are a 1706.67-wide logical canvas (1280 x 4/3) |
| +64 | `v7` | y basis: `(0, -2/H)` |
| +80 | `v8` | NDC anchor `(x, y, 1)`; the shader computes `clip = pos*scale rotated by v6/v7 + v8`, no perspective divide |

The per-frame chain that produces and ships these blocks was traced with a write-watch on a
live slot, then in-process disassembly (2026-09-01/02):

```
la2 evaluator / producers          (+BAFDF quads, +BC213 text, +BB65F, +BCEF3<+BC9A0 - the
                                    innermost caller RVAs name the element *type*)
  -> +666729 -> +666485            (thin wrappers)
  -> +68A410 draw-batch assembler  (builds each record in its own stack frame)
  -> memcpy at +68A933             (rcx = pool cursor, rdx = the record, r8 = size)   <-- the hook
  -> 200 KB ping-pong instance pool
  -> upload helper +7A8CD0, called from +7A583F, whole pool every frame
```

Facts about the pool that shaped the design:

- It is an **append stream**: slot offsets are not stable per element, so nothing can be keyed
  on where a record sits in the pool.
- The upload sends the **whole** 200 KB every frame, but only the region up to the append cursor
  is this frame's; the rest is stale from earlier frames (measured: 178 appends covering
  `+0..+0x1BB0` against 211 anchor blocks in the upload). Reading the uploaded pool to decide
  state - which the zone-based design did - was reading last week's menu.
- Exactly three uploads pass through the helper per frame: the instance pool (204800 B, from
  `+7A583F`), a uint16 index buffer (13776 B, from `+7A946F`, triangle strips with degenerate
  joins) and the vertex buffer (352848 B with the HUD closed, ~390 KB with a wheel open, from
  `+7A94BF`). The vertex data is unaffected by anchor changes - quads are positioned entirely by
  `v8` plus the 12-byte `[u,v][rgba][x,y]` vertices' authored offsets, so moving `v8` moves the
  whole quad rigidly.
- Every 96-byte record on a HUD channel is an anchor block. The non-96-byte records (16/32/48/
  64/80 B) are frame-constant uniforms and colours, identical with the HUD closed and open.

**The shift is therefore applied at the memcpy, one record at a time**, by editing `v8.x` in the
assembler's frame (`rdx`) before it is copied. That is the only place where exactly one live
record is visible, and - the decisive property - where its identity is one stack unwind away.

## 6. Finding identity - the stack unwind

The hook is a SafetyHook mid-hook on the `call memcpy` at `+68A933` (byte-verified: `E8 58 A5
69 00 8B B5 A8 01 00 00`). From there the code cannot see who asked for the record - but the
caller's registers are on the stack, and Windows can restore them: build a `CONTEXT` from the
hook's register snapshot and call `RtlLookupFunctionEntry` + `RtlVirtualUnwind` once. The
unwound context holds the registers the assembler's *caller* was using when it called down:

| Register after one unwind ("frame 1") | Meaning (measured) |
| --- | --- |
| `rbp` | **the owner** - the object doing the drawing. Every gameplay HUD record has `rbp = module + 0x1DE3290` (`kRvaHudOwner`). Pause-menu records come from heap-allocated owners |
| `rsi` | **the channel** - a small index (< 64) that is a fixed id per widget / per selector slot. Same widget, same channel, wheels closed or open, across boots |
| `r14` | the channel's script block (a heap pointer, per boot) |

A second unwind ("frame 2") gives:

| Register | Meaning |
| --- | --- |
| `rbx` | the interpreter position inside the script block the record was drawn for |

and **`rbx(frame 2) - r14(frame 1)` is a stable per-element offset** - constant across frames
and boots for the main-path (`+BAFDF` / `+BB65F`) records. Text records (`+BC213`) and the
ammo ticks give unstable values, and text records also carry a heap `rbp`, so they are never
in the tables.

The key insight is that **the identity is the game's own draw structure**, not anything we
inferred: the engine keeps its UI in per-channel streams selected by a bitmask (the LA2 layer
table; see the la2 notes in [aspect-ratio.md](aspect-ratio.md)), and the channel index and the
script offset are exactly the names the engine uses internally to say "this element".

### The channel map (gameplay HUD owner)

Built by biasing one channel at a time (`chanbias <ch> <dx>`, then a screenshot and a pixel
diff against the unbiased capture) and having the user read what moved:

| Channel | Element | Side |
| --- | --- | --- |
| 0x1D | weapon panel + ammo ticks | right |
| 0x1E | item panel / Solid Eye (becomes the top slot of the item column) | left |
| 0x21 | life bar | left |
| 0x23 | camo ring | right |
| 0x04 | battery bar (under the item panel; anchor `(-0.539,-0.911)`) | left |
| 0x13 / 0x15 / 0x17 | weapon wheel column | right |
| 0x19 | weapon wheel junction | right |
| 0x1B | weapon wheel row | right |
| 0x0E / 0x10 / 0x12 / 0x14 | item wheel column | left |
| 0x16 | item wheel junction | left |
| 0x18 / 0x1A / 0x1C | item wheel row | left |
| 0x26 | CAUTION banner | centred (not in the table) |
| 0x25 / 0x29 / 0x2A | centre-ish elements | centred |
| 0x08 / 0x09 / 0x0A | SE MODE selector cluster | centred (user decision pending) |
| 0x20 | selector overlay, centre-anchored `(0,0)`, only exists while a wheel is open | per element, below |

The pause menu draws on channels 1-4 under heap owners - the same numbers the battery bar uses
under the HUD owner - so the key is the pair `(owner, channel)`, never the channel alone. With
`owner != kRvaHudOwner` nothing moves, which is how menus centre themselves with zero menu
detection.

### The element map (channel 0x20)

Channel 0x20 carries three different things and needed the second unwind. It was mapped with an
**ordinal sweep**: `chanord 0x20 <n> 0.2` biases only the n-th 96-byte record of that channel in
each frame; ten captures, ten diffs, and a `sites` dump that prints each record's element
offset. Each ordinal moved exactly one element:

| Wheel | Element offset | What it draws | Treatment |
| --- | --- | --- | --- |
| item | +0x10F8 | fullscreen dim backdrop | stays centred |
| item | **+0x1950** | selector backing plates: the solid orange selected block and the translucent slot boxes | **left** |
| item | +0x3B38, +0x3C08 | centred title / description / button prompts | stays centred |
| item | +0x3EC0 | SE MODE plate | stays centred (pending) |
| weapon | +0x1BB8 | fullscreen dim backdrop | stays centred |
| weapon | **+0x2410** | selector backing plates | **right** |

The plates were the last visible defect: the item label had moved with its wheel but "there is a
fairly large bright orange box ... extending out to the right, then there's the orange
translucent box ... offset to the right" (user, 2026-09-03). They are centre-anchored quads with
absolute offsets, drawn by the overlay channel, not by the slot channels they visually belong to.

`kHudElements` in the source is this table; `IdentifyAppend` performs the second unwind only
when the channel has element entries, so the cost stays one unwind per record for everything
else.

## 7. The shift

```
factor = outputWidth / (outputHeight * 16/9)     // 4/3 at 3840x1620
shift  = 1 - 1/factor                            // 0.25 NDC = 480 px per side
v8.x  += side * shift                            // side = -1 left, +1 right, 0 leave
```

The constant is exact, not tuned. Layer 1 compressed the canvas so that `x_ndc * outputWidth =
x_safe * safeWidth`; adding `1 - 1/factor` to a canvas-edge anchor puts it on the output edge,
and every anchor on the same side moves by the same number of pixels. So each widget keeps its
**16:9 pixel distance from the screen edge**, and every part of a widget - satellite anchors,
rotated quads, text, ticks - translates rigidly. At `factor == 1` the shift is `0.0` and
`PoolAppendProbe` returns before unwinding anything: strict no-op on 16:9.

Rotated quads (compass arrow, diagonal weapon silhouette, objective marker) fail the strict
axis-aligned block signature; under a known owner and channel the relaxed signature (`v8.z == 1`,
sane basis magnitudes, any rotation) is enough, because the identity already excludes non-UI
data.

## 8. How it was measured - the lab

None of the numbers above came from reading disassembly alone. The workflow, all in
`C:\mgspf_tools\`:

- **Lab mode.** A marker file `MGSPatriotFix.lab` in the game root makes the ASI read
  `MGSPatriotFix.lab.settings` instead of the user's settings (3840x1620 window, `Fix UI
  Aspect=1`, `Aspect Pool Spread=1`, `Fix UI Aspect Corners=0`) and switch on the research
  code. The user's own play configuration is never touched.
- **Harness.** `boot_to_aim.ps1 -LabConfig -LoadSave -SaveIndex 7 -KeepOpen` boots to a safe
  Act 4 save (the rusted-barrel save; the snowfield save can kill Snake mid-test); `shot.ps1`
  captures the client area; the game is closed and the marker removed after every run.
- **Live commands** through `live_anchor.txt`, polled by the ASI, so the game never has to
  restart between experiments ("live editing is going to be the fastest and most comprehensive
  way to test these things" - user): `spread 0|1`, `chanbias <ch> <dx>`, `chanord <ch> <n>
  <dx>`, `chanreset`, `sites` (dump every append of the next frames with unwound registers,
  chain RVAs and node bytes), `uploads <n>` (census of the upload helper), `bufdump` (write the
  three per-frame buffers to disk for offline analysis with `vbdiff.py` / `vbpeek.py` /
  `vbfind.py`), `pooldump`.
- **Pixel diffs.** `uidiff.ps1` diffs each capture against an unbiased reference at half scale
  and prints the bounding box of what changed. One bias, one capture, one diff, one element.
- **The user reads every capture.** No screenshot was treated as fact until the user confirmed
  what it showed. Two misreads were caught this way (below).

Run logs: `C:\mgspf_tools\lab3\` (`sites/`, `chanbias/`, `itm_chans/`, `itm_plates/`,
`itm_recs/`, `itm_uploads/`, `buf/`, `itm_ord/`, `wpn_ord/`, `elem_verify/`).

## 9. Dead ends worth not repeating

- **The "(-1,1) channel-0 canvas block"** was a false positive: the tail of a 32-byte record
  followed by an 80-byte all-ones record, not an anchor. Channel 0 is not a canvas anchor;
  biasing it proves nothing.
- **The solid selected block "growing" under a 0x16 bias** was the selection sweep animation,
  not a bias response. Two captures of an animating element differ on their own; diff against a
  capture taken in the same animation phase, or watch the element that *should* have moved.
- **The weapon-wheel ordinal-0 diff** (a region at the far right edge) was a lamp in the world
  showing through the moved backdrop, not a plate: element +0x1BB8 is the dim backdrop.
- **The two "instance array" upload sites** from the older research (`+7A582B` / `+7A57EA`)
  carry only tiny constant records; the real pool rides the generic helper with its source
  pointer as the fifth argument, on the stack, invisible to a register probe.
- **Reading the uploaded pool for state** sees stale records from earlier frames.
- **Buffer-dump offsets differ between runs**; locate blocks by content (e.g. `v8 == (0,0)` for
  the overlay channel), never by a saved offset.

## 10. What is not done

- **The dim backdrop stops ~240 px short of each screen edge at 21:9** (elements +0x10F8 /
  +0x1BB8). That is the game's own quad size, not the shift; a fix would scale that element's
  `v6.x` basis rather than move it. Not yet decided.
- **SE MODE selector cluster** (channels 0x08/09/0A and element +0x3EC0) stays centred at the
  top of the item wheel. Whether it should travel left with the wheel is the user's call.
- **Channels 0x04 and 0x12 fire under the HUD owner during the title and save screens.** Logged
  every boot; not yet checked for a visible side effect on those screens.
- **Only the Act 4 barrel-save loadout has been measured.** Other HUD states - alerts, codec,
  boss HUDs, the expanded radar, Drebin's shop, cutscenes - need a channel census before
  anything is claimed for them. A channel not in the table simply stays centred, so the failure
  mode is a cosmetic gap, never a wrong jump.
- **The 3D camera is still Vert-** at 21:9 (AR-002), and 4:3 has not been tested with layer 2
  (the shift is x-only; 4:3 wants the same rule rotated).
- **The zone-based code paths** (`Fix UI Aspect Corners`, the pool-state detector, the la2
  strcode corner table) are still in the tree, off; they get deleted once the identity path has
  been through a full play session.

## 11. Reference

| Item | Value |
| --- | --- |
| layout converter | `+0x439810`, inline hook, `ConvertLayout` |
| window size globals | `+0x1B00460` / `+0x1B00464` |
| pool append memcpy | `+0x68A933` (mid hook `PoolAppendProbe`; `rcx` dst, `rdx` record, `r8` size) |
| draw-batch assembler | `+0x68A410` |
| upload helper / pool caller | `+0x7A8CD0` / `+0x7A583F` |
| index / vertex upload callers | `+0x7A946F` / `+0x7A94BF` |
| gameplay HUD owner | `+0x1DE3290` (frame-1 `rbp`) |
| anchor block | 96 B: `v6` +48, `v7` +64, `v8` +80 |
| element type by innermost producer | `+BAFDF` quads, `+BC213` text, `+BB65F`, `+BCEF3<+BC9A0` |
| shift | `1 - 1/factor` per side; `factor = W / (H*16/9)` |
| tables | `kHudChannels` (owner+channel), `kHudElements` (owner+channel+element offset) |

Attribution: the layout converter RVA and object field layout are mgs4Ultra120's research
(github.com/drbermejor/mgs4Ultra120); the la2 archive-read signature is from cipherxof's
MGS4-ModLoader (MIT). No code from either is used. Everything from section 5 onward was
measured in this project.
