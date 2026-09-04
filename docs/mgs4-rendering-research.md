# MGS4 rendering research notes

Reverse-engineering notes behind the MGS4 resolution, shadow, and window-size features in
this fix, plus the UI coordinate bug that supersampling exposes.

Verified against **METAL GEAR SOLID 4 (Master Collection)**, D3D12 backend, on 2026-08-28/29.
Absolute addresses are `mgs4.exe` RVAs from that build. Byte signatures are the durable part;
offsets are quoted to make the notes concrete.

---

## 1. The executable and engine

| Property | Value |
| --- | --- |
| Renderer | **bgfx**-based (engine identifies itself as "VTS"), D3D11/D3D12 selectable in-game |
| Windowing | GLFW |
| Protection | Steam DRM — a `.bind` section, with `.text` **encrypted on disk** |
| Image size | 605 MB (`SizeOfImage` = `0x241BE000`) |

Two consequences shaped every investigation:

**`.text` is encrypted on disk.** Static analysis of `mgs4.exe` finds nothing at the
interesting addresses. All disassembly work has to be done against a **live memory dump**
taken after the Steam stub decrypts. `.rdata` is *not* encrypted, so string searches against
the file on disk work fine.

**`.data` is enormous and mostly zero-initialised** — virtual size `0x224FB238` (~574 MB)
against a raw size of `0x26D200` (~2.5 MB). A partial memory dump captures almost none of the
engine's globals. Several early cross-reference hunts failed for this reason alone: the data
simply was not in scope.

The game also cannot be launched under a capture tool or debugger — it exits within about a
second when it is not started by its own `launcher.exe`. Attaching after startup is too late
to hook D3D12. See [§6](#6-tooling-notes) for the way around this.

---

## 2. Render resolution

### 2.1 The key insight

The engine keeps **two independent resolutions**, exposed as separate config keys:

| Key | Global | Meaning |
| --- | --- | --- |
| `render.bufferSizeX` | `+0x1B00000` | Internal render buffer width |
| `render.bufferSizeY` | `+0x1B00004` | Internal render buffer height |
| `render.windowSizeX` | `+0x1B00460` | Window / swapchain width |
| `render.windowSizeY` | `+0x1B00464` | Window / swapchain height |

Because they are separate, raising **only** `bufferSize` renders internally at a higher
resolution while the window and swapchain stay native — the engine scales down on output.
That is supersampling, and it works even when the display cannot exceed its native resolution
(e.g. a 4K panel limited by DSC).

`render.bufferSizeX/Y` is also what the engine's dynamic resolution system takes as its
**maximum**; `FUN_7ff736c40be0` calls the DRS initialiser with `(bufferSizeX, bufferSizeY, 60)`.

### 2.2 Where the values are parsed

The engine's config init (`+0x3BD90`) parses four keys into globals as a run of near-identical
`CALL strtol` / `MOV [global], EAX` pairs. There are **two** such write blocks; the second one
is the one that matters:

```
+0x3C09F  MOV [render.windowSizeX], EAX
+0x3C0AA  MOV [render.windowSizeY], EAX
+0x3C0B5  MOV [render.bufferSizeX], EAX
+0x3C0C0  MOV [render.bufferSizeY], EAX
+0x3C0C6  <- hook installed here, after all four stores
```

Signature used to locate it:

```
E8 ?? ?? ?? ?? 89 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 89 05 ?? ?? ?? ?? E8 ?? ?? ?? ??
```

Relative to the match: displacements at `+0x07`/`+0x12` resolve to `windowSizeX/Y`,
`+0x1D`/`+0x28` to `bufferSizeX/Y`, and the hook goes at `+0x2C`.

> **Trap.** An early version read `+0x07`/`+0x12` — the *window* globals — and hooked at
> `+0x16`, which sits *between* the two pairs. The override was applied and then immediately
> overwritten by the game's own `bufferSize` stores two instructions later. Everything
> appeared to work (the log showed the write) while having no effect whatsoever.

### 2.3 Signature validation

That signature is weak: 7 fixed bytes out of 27, all common opcodes. It matches **ten**
places in `.text` — five inside the parse run itself, because the sequence repeats every
11 bytes and the pattern overlaps itself:

```
+0x3C09A  <- intended
+0x3C0A5  +0x3C0B0  +0x3C0BB  +0x3C0C6   (self-overlap)
+0xD0EC27  +0xE39077  +0xE6F303  +0xE6F30E  +0x14CFD7C
```

Taking the first match happens to be correct, but only by address ordering — and since the
resolved displacements are *written to*, a build with an earlier match would put render
dimensions into unrelated globals.

Structural validation is **not sufficient**: requiring two adjacent in-module writable dwords
still accepts `+0xE6F303`, an unrelated but similar config-parse run.

The fix resolves `render.bufferSizeX` **semantically** — the config key string is referenced
exactly once in the code, and the first `MOV [rip], EAX` after that reference is the global
itself. Only the signature match whose third store writes that exact address is accepted, and
it fails closed if the string or store cannot be found.

### 2.4 Clamping

Dimensions are rounded down to a multiple of 4 (the engine derives half- and quarter-resolution
buffers from them) and clamped. The ceiling depends on whether the UI fix ([§5](#5-the-ui-16-bit-position-wrap))
is enabled:

| UI fix | Max buffer width | Reason |
| --- | --- | --- |
| off | **4092** | UI positions are 16-bit; past 4095 the interface breaks |
| on | **8192** | D3D12's 16384 texture cap against the engine's double-width buffer |

---

## 3. Shadow resolution

Shadow map size is **not** a render config key. It comes from the engine's scalability system,
loaded from the encrypted `mgs4.scalability_PC.ecf`.

`SavedSettings.shadowQuality` (0–4) drives two scalability groups via `FUN_7ff736724080`:

| Group | Setting |
| --- | --- |
| 4 | `ShadowSampleCount` |
| 5 | `ShadowBufferSize` |

The value is scaled where it is **parsed**, before anything is allocated, so every consumer —
allocation, viewports, projection, and the `vts_shadowbufferSize` shader uniform — derives from
the raised number consistently:

```
+0x142BD5  TEST DIL, DIL              ; did the key match "ShadowBufferSize"?
+0x142BD8  JZ   <skip>
+0x142BDA  LEA  RCX, [RBX + 0x30]
+0x142BDE  CALL <parse int>           ; result in EAX
+0x142BE3  MOV  [RBP-0x60], 5         ; group 5 — hook here, EAX rewritten
+0x142BF2  MOV  [RBP-0x50], EAX       ; engine stores our value
```

Signature (unique — one match in `.text`):

```
48 8D 4B 30 E8 ?? ?? ?? ?? C7 45 ?? 05 00 00 00 49 8B 55 08
```

The literal `5` is what makes it safe: the branch immediately above is byte-identical except
it encodes `4`, for `ShadowSampleCount`.

The hook fires **once per quality tier** in the scalability table, so the scale applies whichever
Shadow Quality is selected in-game:

| Tier | Base | At 300% | Atlas |
| --- | --- | --- | --- |
| low | 512 | 1536 | 1536 × 3072 |
| medium | 1024 | 3072 | 3072 × 6144 |
| high | 2048 | 6144 | 6144 × 12288 |

The atlas is allocated as `width × 2·width`, so the width is clamped to 8192 to stay inside
D3D12's 16384 limit. Cost is quadratic: at 300% the atlas goes from 32 MB to 288 MB, plus a
second shadow buffer at 144 MB.

---

## 4. Dynamic resolution

The engine ships a dynamic resolution system — hence the "up to 4K" wording in press material.
Its debug panel (`VTS BGFX Debug GUI` → `DynamicResolution`) exposes *Viewport percentage*,
*Buffer* vs *Adjusted* dimensions, GPU timings, and a *Panic / Budget / HeadRoom / Margin* set.

| Item | Location |
| --- | --- |
| Enabled flag | `+0x22A8D78` (one byte) |
| State struct | `+0x22A8B80` |
| Buffer width / height | struct `[0x70]` / `[0x71]` |
| Adjusted width / height | struct `[0x72]` / `[0x73]` |
| Min / max scale | struct `[0x76]` = 0.5, `[0x77]` = **1.0** |
| Target frame time | struct `[0x6A]` = 60 |

Max scale of exactly 1.0 confirms DRS only ever scales **down**. Relevant shader uniforms:
`vts_dynResUV`, `vts_dynResFactor`, `vts_viewportSize`, `vts_framebufferSize`.

---

## 5. The UI 16-bit position wrap

### 5.1 Symptom

Above roughly a 4K-wide render buffer, the aiming reticle disappears completely. Some menu
text also breaks. The rest of the HUD is unaffected.

> **The reticle is fixed.** Verified by measurement and by several hours of ordinary
> play at a raised internal resolution.
>
> **The menu text claim is retracted - it was never evidenced.** It appears in the
> first version of these notes and was repeated afterwards without a measurement, a
> capture, or a reproduction behind it, unlike everything else in this section.
> Reported observation is that broken menu text was only ever seen *with the shader
> workaround enabled*, which fits: that workaround had to infer which coordinates had
> wrapped, and its edge margin could misfire on text legitimately near a screen edge.
> On that reading it was an artefact the workaround created, not a symptom of the wrap.

### 5.2 What it is not

Ruled out by measurement, in order:

| Suspect | How it was eliminated |
| --- | --- |
| Failed texture allocation | `HRESULT` checked on every `CreateCommittedResource` / `CreatePlacedResource` — all clean |
| Missing or mis-sized render target | Full render-target diff between working and broken captures — everything scales correctly |
| Draws culled upstream | Pass-for-pass comparison: reticle pass present in both, with the identical 10 draws |
| Wrong viewport | Logged — `7680x4320`, correct |
| Empty or truncated scissor | Logged — `L=0 T=0 R=7680 B=4320`, full buffer |
| The engine's double-width buffer hitting 8192 | A 4096-wide buffer *flickers* rather than passing cleanly, which that theory predicts it should |

### 5.3 Root cause

UI geometry uses `POSITION0` in **`R16G16_SINT`** — 16-bit signed screen coordinates at
**1/16 pixel**. The largest coordinate expressible is therefore:

```
32767 / 16 = 2047.94 px
```

The reticle is positioned at the **centre of the screen**, so its coordinate is `width / 2`.
It overflows once the render buffer is wider than **4095**.

Confirmed quantitatively from a PIX capture at 7680×4320. The vertex shader emits
`SV_POSITION` at NDC `(-1.07, +1.90)`, which is screen pixel `(-256, -1936)` — exactly what
16-bit wraparound predicts:

| Axis | centre px | × 16 | wraps to int16 | ÷ 16 |
| --- | --- | --- | --- | --- |
| X | 3840 | 61440 | `61440 − 65536 = −4096` | **−256** |
| Y | 2160 | 34560 | `34560 − 65536 = −30976` | **−1936** |

The draws are still issued, with correct viewport, scissor and render target. The geometry is
simply rasterised far off screen. Nothing errors, because an integer wrap is silent.

This also explains the boundary behaviour precisely:

| Buffer width | Behaviour |
| --- | --- |
| ≤ 3840 | stable — comfortably under the limit |
| **4096** | **flickers** — sitting exactly on it, with dynamic resolution nudging the effective width either side each frame |
| ≥ 4800 | stable but permanently missing |

### 5.4 Why most of the HUD is fine

UI elements are authored in a **virtual 1280 × 720 space** and scaled to the render buffer:

```c
// FUN_7ff736adc810 and friends
pixel_x = (virtual >> 4) * bufferSizeX / 0x500;   // 1280
pixel_y = (virtual >> 4) * bufferSizeY / 0x2d0;   //  720
```

Ordinary HUD elements keep small virtual coordinates and never overflow. Only elements
positioned in **buffer pixels near screen centre** wrap. The reticle is the confirmed case;
menu text was also listed here originally, but see the retraction in 5.1 - that was never
measured.

Two later corrections to this picture, both from §5.6: the reticle is **several quads**, not one,
and they spread apart as the crosshair expands; and the sniper scope's vignette, while it never
wraps, is anchored at a screen corner and so sits on the detection boundary. Between them they
set the resolution ceiling.

Note the `>> 4` happens *before* the multiply, so these functions output plain `int32` pixels.

**The inverse conversion also exists, and is where the reticle is lost.** Around `+E39816` the
engine goes the other way - buffer pixels into virtual space - with the same two constants:

```
cvttss2si ecx, xmm0        ; float -> int32, full precision
movsx     edx, cx          ; keep only the low 16 bits   <-- the defect
lea       eax, [rdx+rdx*4] ; x5
shl       eax, 8           ; x256, so x1280 total
idiv      [render.bufferSizeX]
```

with `imul eax, r, 0x2D0` (x720) against `bufferSizeY` on the Y routes. So both directions are
present: `FUN_7ff736adc810` maps virtual to pixels, and this one maps pixels back to virtual. The
truncation is on this side, which fits the symptom - the reticle is the element positioned in
buffer pixels and therefore the one that has to be converted inward.

> **Corrected.** This section originally claimed that "no function in the binary both reads the
> render size globals and performs a multiply-by-16 into a 16-bit store". That is wrong twice
> over. `mgs4.exe+4E14E0` does both — just in unrelated branches, which is why it is a false
> positive rather than the conversion (§5.8). And the search itself was too narrow: the ×16 is a
> float constant, not a shift, so `SHL reg,4` was never the right thing to look for.
>
> The guess that the reticle "is most likely a projected world point" turned out to be right, and
> is now established rather than speculation — see §5.7, which supersedes this section's framing.
> The real split is not "most of the HUD" versus a few unlucky elements, but **static UI**
> (virtual space, safe by construction) versus **dynamic markers** (projected to screen pixels,
> able to overflow).

### 5.5 The fix

Since the wrap happens on the CPU, it cannot be prevented from our side — but it is a **single**
wrap of exactly 65536 units (4096 pixels), which in NDC is `2 × 4096 / dimension`, so it can be
undone.

The game's UI vertex shader — DXBC checksum **`c0f6fdb2-73bd444d-069170c9-a85fc8a1`** — is
replaced at `CreateGraphicsPipelineState` with a corrected copy, compiled at runtime once the
render size is known. The body is the game's own shader; only the correction is added. Verified
against `fxc`: input and output signatures, including component masks, are identical.

```hlsl
r0.xy  = (int2)v1.xy;
r0.xy  = v3.xy * r0.xy;
r0.yzw = v7.xyz * r0.yyy;
r0.xyz = r0.xxx * v6.xyz + r0.yzw;

float3 anchor = v8.xyz;          // the translate — this is what wrapped

// Out past the edge, but not so far it cannot be a wrap, and not so close it is
// merely an element anchored at the edge.
bool wrappedX = anchor.x < -1.0f - MARGIN_X && anchor.x > LIMIT_X;
bool wrappedY = anchor.y >  1.0f + MARGIN_Y && anchor.y < LIMIT_Y;

#if CORRECT_X && CORRECT_Y
if (wrappedX && wrappedY)
{
    anchor.x += WRAP_X;          // 2 * 4096 / width
    anchor.y -= WRAP_Y;          // 2 * 4096 / height
}
#elif CORRECT_X
if (wrappedX) { anchor.x += WRAP_X; }
#elif CORRECT_Y
if (wrappedY) { anchor.y -= WRAP_Y; }
#endif

o0.xyz = anchor + r0.xyz;
o0.w   = 1;
```

Four details matter, all found the hard way:

**Correct the translate, not the final position.** The translate is shared by all six vertices
of a quad, so the quad moves as a unit. Correcting `o0` instead **tore** any quad straddling a
screen edge, because only some of its vertices qualified — visible as UI smearing horizontally
across the screen.

**Require every axis that can wrap to be out of range**, not either one. A centre-screen element
that genuinely wrapped is out of bounds on *both* axes; UI merely clipped by a screen edge is out
of bounds on one with an ordinary value on the other. Testing a single axis **teleported**
legitimately off-screen elements to the opposite side of the screen. Whether each axis can wrap
is computed at compile time from the resolution (`dimension / 2 > 2047`), so the test degrades
sensibly at other resolutions.

**Do not patch at all when nothing can wrap.** The per-axis flags originally only chose *which
branch* the shader took; with both false it still fell through to the single-axis branch and
corrected anyway. At a 3840 × 2160 buffer — reachable as a 1080p window at 200% — nothing can
wrap, yet every element anchored off the left or top edge was thrown right and down across the
screen. The sniper scope's vignette is anchored exactly there, so the scope broke while the
identical buffer at stock 4K was fine. The shader is now left alone entirely unless some axis
can actually overflow.

**Ignore anchors that merely straddle the edge.** An element anchored *at* a screen corner sits
on the boundary by construction, and the scope's vignette lags behind fast camera movement by a
growing but bounded distance — enough to put it marginally outside and trip the test. A real wrap
is never marginal: at 7680 × 4320 it lands 256px past the left edge and 1936px past the top. The
`MARGIN` terms ignore the first stretch past each edge, derived as half the distance a wrapped
centre coordinate travels (`(4096 - dimension/2) / 2`), overridable via the undocumented
`UI Edge Margin (px)` setting.

If compilation or pipeline creation fails, it falls back to the original shader and the game runs
normally.

### 5.6 Known limitation, and the resolution ceiling it imposes

This is a **heuristic, not an inversion**. The wrap destroys information: a UI element far enough
right that its true coordinate exceeds ~4096 px wraps to a *positive* value indistinguishable
from a legitimate one, and no shader-side test can recover it.

The whole method rests on one quantity — how far past the screen edge a wrapped centre-screen
coordinate lands:

```
headroom = 4096 - width/2
```

That gap is the only thing separating a genuine wrap from UI legitimately anchored at the edge,
and **two different HUD elements spend it from opposite ends**:

- **The sniper scope's vignette** is anchored at a screen corner and lags behind fast camera
  movement, drifting *outward* past the edge. The margin must exceed that lag — measured between
  64px and 128px at a 7680-wide buffer.
- **The crosshair** is not one quad. Its arms are separate quads that spread outward from centre
  as the crosshair expands, so each has its own coordinate. An arm whose coordinate passes 4096
  wraps to a positive value and is gone. The margin must therefore stay *below*
  `headroom - spread`.

At **7680 wide** the headroom is only 256px. Static crosshair spread already exceeds the margin
the scope needs, and walking — which expands the crosshair — pushes arms past 4096 outright, where
no threshold of any value can help. There is no setting that satisfies both.

| scale at 4K | buffer | centre | headroom |
| --- | --- | --- | --- |
| 100% | 3840 | 1920 | no wrap at all |
| 125% | 4800 | 2400 | 1696px |
| 150% | 5760 | 2880 | 1216px |
| 175% | 6720 | 3360 | 736px — measured working |
| 200% | 7680 | 3840 | 256px — measured broken |

Hence `iMaxInternalBufferWidth = 6720` whenever the UI fix is on. The ceiling is a property of
the method, not a tuning failure: it tightens as the buffer approaches 8192, where a centre
coordinate wraps to exactly the screen edge and the two cases become identical. The patch refuses
outright below 64px of headroom rather than guess.

A proper fix is upstream — make the UI compute against the **window** size rather than the render
buffer. NDC is scale-invariant, so the UI lands in the same place and nothing wraps at any scale,
retiring the shader patch entirely. That is what `bRedirectUiCoordinateSpace` and the site
bisection in §5.8 exist for; it is not yet identified. Alternatively, CPU-side, 1/4-pixel
precision instead of 1/16 would raise the ceiling to `32767 / 4 = 8191 px` — but that needs the
conversion site, which was never found, and the convention is presumably shared with the code
that reads those values back.

### 5.7 Two UI coordinate systems, and which one actually breaks

The single most clarifying observation in this whole investigation came from watching the game
rather than the code: **the static UI has never broken.** Menus, HUD panels, the ration box, the
weapon box — all correct at every supersampling factor. Only elements that *move* — the
crosshair, the Solid Eye enemy/ally overlays, the sniper scope — misbehave.

That is because there are two paths, and only one of them can overflow.

**Static UI is authored in a virtual 1280×720 space.** A rectangle is stored in render-buffer
pixels (`mgs4.exe+4E1D00` builds the full-screen rect from `bufferSizeX/Y`, then a sub-rect via
`imul` + divide-by-1280), and the getter at `+4E14E0` converts it back:

```
field × 1280 ÷ bufferSizeX × 16      → 1/16th of a virtual pixel
```

The buffer size **cancels**. The result is capped at `1280 × 16 = 20480`, comfortably inside
int16, at any resolution. This path is safe by construction and needs no fix. Constants confirmed
by reading them live: `+1664D18 = 16.0f`, `+1664D1C = 720.0f`, `+1664D20 = 1280.0f`.

**Dynamic elements are world positions projected to screen**, so their coordinate is in real
buffer pixels and grows with the render buffer. At 7680 wide, screen centre is `3840 × 16 =
61440`, which wraps — matching the reticle measurement in §5.3 exactly.

This retroactively explains the shader heuristic: it only ever corrected projected elements near
screen centre, which is the entire population of things that can wrap.

**It also invalidates the coordinate-space redirect.** `bRedirectUiCoordinateSpace` repoints the
*static* path at the window size — a subsystem that was already resolution-independent. Doing so
merely desynchronises the writer from the reader at `+4E14E0` and drags the working UI into the
top-left corner at half scale. Repointing the reader's divisors too (`+4E1518`, `+4E154B`,
`+4E1589`, `+4E15C4`) keeps them in step but still fixes nothing, because nothing there was
broken. The setting is a dead end and is kept only as a record of the experiment.

### 5.8 Finding the projection-to-fixed-point conversion

An earlier hunt for `SHL reg, 4` beside a render-size read produced exactly one hit, `+4E14E0`,
which turned out to be a false positive: it reads the render size and shifts left by 4, but in
*unrelated branches*. The ×16 is a float constant, not a shift, so the correct fingerprint is
code that reads a render-size global **and** `16.0f`.

`bScanFixedPointSites` disassembles all 23 MB of `.text` with Zydis, in-process (`.text` is
Steam-encrypted on disk, so this is the only way to read real code without a memory dump), and
reports clusters where both appear within 256 bytes. Of 818 references, **15 clusters** qualify.
Ten of them contain the normalising divide and are provably safe. The standard safe idiom:

```
mulss     xmm, [16.0f]
cvttss2si eax, xmm
lea       eax, [rax+rax*4]   ; ×5
shl       eax, 0x08          ; ×256   → ×1280
idiv      [bufferSizeX]      ; ÷ buffer → virtual × 16
```

The clearest suspect so far is **`mgs4.exe+529FB3`**, which multiplies by 16 with no
normalisation at all:

```
movss     xmm0, [rbx+0xA0]   ; float x
mulss     xmm0, [16.0f]
cvttss2si ecx, xmm0
mov       [rax+0x1C], ecx    ; int32
mulss     xmm1, [16.0f]      ; y
mov       [rax+0x20], ecx    ; int32
```

The fields are 32-bit, so the truncation to int16 happens further downstream, where these reach
the vertex buffer. Finding that store is the remaining step. **Unresolved at time of writing.**

### 5.8b Hunting the producer: what has been ruled out

The conversion that wraps has not been found. What is established, and what is eliminated:

> **Resolved.** The conversion is `movsx r32, cx` - a register-level narrowing, never a
> memory write, which is why every technique below came back empty. Those were correct
> answers to the wrong question. See `crosshair-investigation-state.md` §0a.


**Established.** The crosshair's vertex data is **written once and never touched again**. A
hardware watch (read *and* write) on memory holding the wrapped value fires zero times across
aiming, lowering, and re-aiming, while a self-test watch on a mod-owned variable fires normally -
so the watch works and the silence is real. The data is in a GPU-visible buffer read by DMA,
which debug registers cannot observe. The wrapped value sits at a stable offset within its
allocation across runs (`…D44`, `…206`), i.e. it is built once at load, not per frame.

**Eliminated:**

| Hypothesis | How it was tested | Result |
| --- | --- | --- |
| One of the 15 float→int16 conversion sites | Hardware execute breakpoints, 4 at a time | All dead during gameplay, or writing small values (`6654`, `15`) that never truncate |
| The producer reads `bufferSizeX` while aiming | Hardware watch on the global, list cleared on F9 | 21 readers; none perform fixed-point conversion (no `16.0f`) |
| The producer reads a cached float copy | Scan for a persistent `(7680.0, 4320.0)` float pair, watch reads | No stable pair is ever read by mgs4.exe code |
| Setting `bufferSize` via `.ecf` avoids it | Config edit, with and without `dynamicResolution` | The engine overwrites `bufferSize` with `windowSize` during init regardless - the config route cannot raise internal resolution at all |

**PAGE_GUARD was tried and abandoned.** Guarding memory to catch the write works in principle -
a single guarded region produced 119 correctly attributed writes - but not in a form that catches
this particular one:

- Guarding the two pages around the value: zero writes. Nothing rewrites it once it exists.
- Guarding its whole containing region: works, but the value is written before its address can be
  known, so there is nothing to point at in time.
- Guarding every private heap region so the guard is already armed when the value is created:
  **froze the game solid, three times.** Thread stacks must be excluded (a guard page on a live
  stack breaks stack growth), but excluding them is not sufficient - once enough of the heap is
  guarded, the exception dispatch path itself cannot run without faulting, and the process
  livelocks. An auto-disarm timer does not help because the thread meant to fire it is wedged too.

Re-arming the guard on the aim button, so it is fresh at the moment the crosshair is built, is a
good idea and worth recording - but it locked up as well.

**Was open, now resolved.** No safe technique was ever found for catching the write - because
there is no write. The truncation happens in a register. What had been left to try:

Broadening the fingerprint to cover packed stores - `packssdw` / `packusdw`, and 32/64/128-bit
stores following a float-to-int conversion with the scale constant nearby - yields **86 packs and
103 wide stores**.

**Software breakpoints make a list that size testable.** Debug registers give four watchpoints;
an `INT3` costs one byte at a known instruction boundary, so every candidate can be armed at once
(`Trap Fixed Point Sites`). Of 189 candidates, **5 execute during gameplay**:

| Site | What it does |
| --- | --- |
| `+42785C` | 32-bit stack store; reads 16-bit fields and multiplies by 1/16 - a consumer |
| `+4B62AC`, `+4B62B2` | `value * 16.0f` -> `cvttss2si` -> **32-bit** store |
| `+6A27DC` | `value * 16.0f`, converted back to float, stored as float |
| `+F70C12` | 32-bit store to `[rax+0x20]` - the same coordinate field written at `+529FDE` |

None of them truncates to 16 bits. They produce and consume 1/16 fixed point at 32-bit or float
precision throughout.

**Integer narrowing was then ruled out too.** If the coordinate is already an int32 in 1/16 units
in `[obj+0x1C]`, packing it into an `R16G16_SINT` vertex would be plain integer narrowing with no
float involved - invisible to every fingerprint above. Scanning for a 32-bit load from those
offsets followed by a 16-bit store of the same register finds **zero** instances once the tracker
correctly invalidates a register that something else overwrites, and rejects indexed accesses
that merely share the displacement. The 33 hits before those two corrections were all register
reuse; four were disassembled and every one was a coincidence, including a struct initialiser
storing the literal `0x3E38`.

**16-bit arithmetic was ruled out as well.** A wrap can happen in the computation rather than at
a store - `imul ecx,eax / shl cx,4 / mov [rdx],cx` at `+4E160B` truncates in the `cx` operand size
itself. Filtering for a multiply by 16 at 16-bit width feeding a store gives 12 sites; trapping
them shows the live ones store values like `30720` (**1920px - half the 3840 window**), `17664`
and `17600`. They work in *window* space, where centre screen is comfortably inside int16. That
is why stock 4K has always been fine, and it means none of them is the broken path.

**And 16-bit reads of the coordinate fields.** With no truncating store anywhere, the truncation
could be a 16-bit read of a 32-bit field. There are 288 such reads of `[reg+0x1C]`/`[reg+0x20]`;
trapping all of them and printing the full 32 bits at each address finds nothing holding a
wrappable coordinate. The values that look like wraps are either two adjacent int16 fields (upper
half holding a small integer) or sentinels like `0xFFFF`.

### 5.8c Reading the memory instead of guessing the layout

Every approach above assumed a layout. Dumping the memory around a wrapped coordinate shows what
is actually there:

```
+0 | 00 F0 00 87 00 4D FF 51 FF F1 00 59 |
     -4096 -30976  19712 20991  -3585 22784      (as int16, 1/16px)
     -256px -1936px  1232px 1312px  -224px 1424px
```

A dense array of int16 pairs, all plausible screen coordinates, with the wrapped reticle position
sitting in it. **This is vertex data** - and no int32 within +-128 bytes holds `61440`, so the
untruncated value is not retained anywhere near it. Whatever produced this had already wrapped.

**Guarding the upload buffers does not work.** The obvious next move - `PAGE_GUARD` each D3D12
upload buffer at creation, so the single write that fills it identifies itself - fails silently:
`VirtualProtect` reports success on 40 buffers and **not one fault is ever delivered**, reads or
writes. The memory is driver-mapped and the protection does not stick. Two runs reported "0
written by the CPU" before a read counter was added and showed the guards were never firing at
all; without that control the result reads like a finding when it is an absence of measurement.

**Where that leaves it.** The conversion is not any of: a float-to-int16 store, a packed 32-to-16
store, an integer narrowing from `0x1C`/`0x20`, a 16-bit read of those fields, or 16-bit multiply
arithmetic in any live code path. The value arrives in vertex data already wrapped, from code
that no memory-level probe available here can catch.

### 5.8d The UI rendering path, read rather than guessed

Reading the code the draw stack points at, one hop at a time, maps the structure properly.

**The draw loop is `mgs4.exe+79F2B0(container, commandList, layer)`.** It walks an array of
**104-byte draw records**:

```
+79F387  mov  rdi, [rcx+r14*8+0x18]   ; record array for this layer
+79F39D  add  rdi, 0x50
+79F3C5  call [rax+0x130]             ; slot 38: SetGraphicsRootConstantBufferView, root param 2
+79F40B  call [rax+0x160]             ; slot 44: IASetVertexBuffers(0, 5, record)
+79F429  call [r10+0x60]              ; slot 12: DrawInstanced
+79F42D  add  rdi, 0x68               ; next record
```

| Record offset | Contents |
| --- | --- |
| `0x00`-`0x4F` | **five** `D3D12_VERTEX_BUFFER_VIEW`s (16 bytes each) |
| `0x50` | constant buffer GPU address |
| `0x58`-`0x67` | the four `DrawInstanced` arguments |

Five streams explains the shader's inputs: the int16 `POSITION` and the float translate the fix
corrects come from *different* buffers.

**The vertex buffer is a 3 MB default-heap pool.** Logging the streams at a UI draw reports
stride 12, size 3145728, and `<not in a mapped buffer>` - it is GPU-only memory the CPU cannot
write, filled by copy from staging.

That single fact explains every negative result in §5.8b at once. The hardware watches, the page
guards and `ResolveVertexData` were all pointed at memory the CPU never touches. Those probes
were reporting an absence correctly; the mistake was reading each silence as a defect in the
probe and building another one.

**The container lives at `+0x14FC8`** in the renderer object (`+7A5D97` loads it before calling
the draw loop). Enumerating every instruction that addresses that offset gives 11 references:
`+798959` and `+7A1799` are construction and a 4096-entry reserve, the rest are the submit path.
None appends records, so the container is passed by pointer to whoever fills it.

`+7A6914` is the closest yet - it takes the container into `rsi` alongside a cursor at
`[rbp+0xA8]` bounded by `[rbp+0x168]`, which is what walking the vertex pool looks like.

**Was open, now resolved - and the premise was wrong.** This assumed the coordinates are written
to staging by code that narrows them. The narrowing happens earlier and in a register; what
reaches staging is already-converted data. Original note follows.

The coordinates are written to staging memory by code not yet identified, then
copied into the pool. Hooking `CopyBufferRegion` would name the staging buffer, which *is*
CPU-visible - though note `PAGE_GUARD` does not work on that memory either, so it would give the
data rather than the writer.

**Tooling note.** Four separate probes returned nothing because of defects in the probe rather
than absence of the thing being measured: PIX captures with no draw calls (frame interpolation),
a self-test written by the one thread arming skips, a watch armed with no exception handler
registered, and a watch armed only on threads that existed at arming time. Every negative result
from a new probe should be treated as unproven until a positive control fires.

### 5.9 Both UI vertex shaders

Logging every 6-vertex, 12-byte-stride draw during gameplay shows **two** vertex shaders drawing
UI quads, not one. Both are now corrected:

| checksum | translate | notes |
| --- | --- | --- |
| `c0f6fdb2-73bd444d-069170c9-a85fc8a1` | `v8` (TEXCOORD7) | has a TEXCOORD3 input |
| `4a7feca5-3e398a0a-46d540aa-49587443` | `v7` (TEXCOORD7) | no TEXCOORD3; `cb0[9]` |

The second is the same construction with two differences: the translate is `v7`, and its tail
picks a constant-buffer entry from the sign of the final position:

```hlsl
o0.xyz = r0.xyz;
r0.xy = cmp(float2(0,0) < r0.xy);
...
o3.xyzw = cb0[r0.x+0].xyzw;
```

That selection is left exactly as the game wrote it - correcting the anchor *before* it runs is
what makes it choose the right entry, so the wrap was previously corrupting a colour lookup as
well as a position.

Shaders come from the community dump, named by their checksum directly (not byte-swapped, which
is the convention in a different dump). Their call stacks are identical
(`+0x79F42D <- +0x7A5DA3 <- +0x76628F <- +0x7663D7 <- +0x7664AE`) and lead to the generic
draw-submission path, not to whatever positioned them.

**Patching the second shader does not lift the resolution ceiling.** Retested at 200% with both
corrected: still broken. That confirms §5.6 - the limit is the wrap arithmetic, not a missing
patch, and the 6720 cap stays where it is.

### 5.10 Other potentially affected shaders

Of 597 shaders in the community dump:

- **20** take `POSITION` as an integer — the vulnerable `R16G16_SINT` format
- **14** of those build `SV_POSITION` by adding a per-vertex translate, structurally identical
  to the bug fixed here, differing only in which register carries it (`v5`, `v7`, `v8`)
- **1** is patched

So roughly 13 shaders carry the same latent bug. They have not been observed breaking, most
likely because the affected elements are not centre-anchored or were not exercised during
testing. Plausible candidates: codec calls, Solid Eye / iPod overlays, cutscene subtitles, the
Mk. II interface, boss health bars.

Recommended next step is to log every vertex shader checksum the game actually creates across a
range of scenarios, and only patch the ones that genuinely appear.

---

## 5b. Other engine settings worth overriding

Three values the game reads but does not expose, all hooked the same way — at the instruction
that stores the parsed value, substituting ours.

| Setting | Site | Notes |
| --- | --- | --- |
| `ShadowSampleCount` | `+142B39` | Shadow edge filter taps; highest preset ships 7. The branch immediately above `ShadowBufferSize` (`+142BDA`), distinguished only by the group id stored into the same slot — 4 rather than 5 |
| `render.fxaa` | `+65CC49` | Read through a getter that appends a platform suffix to the base key, so there is no single global; the getter is setting-specific and leaves the answer in EAX |
| `render.fxaaParam` | `+663DA6` | FXAA quality: `Slow(0) / Medium(1) / Fast(2)`, slower being better. Parsed as a double, narrowed with `cvtsd2ss`, stored as a float — the hook sits on that store |

Anisotropy is the exception: it arrives as part of a compound value
(`TG_Default=MipBias=0,MaxAniso=2`), once per texture group. Rather than parse that, the fix
hooks `ID3D12Device::CreateSampler` (vtable slot 22) and raises `MaxAnisotropy` on samplers that
already request anisotropic filtering. Simpler, covers every group, and leaves point/linear
samplers alone so the intended look is unchanged.

FXAA is worth keeping *on* alongside supersampling: extra resolution resolves geometry edges but
does nothing for shader aliasing or specular sparkle.

---

## 5c. The .ecf config files

`MGS4\config\*.ecf` are INI text XORed with the 28-byte ASCII key `MGS4ConfigFileSecureKey@2024`,
with the index advancing one extra position each full cycle:

```python
key = b'MGS4ConfigFileSecureKey@2024'
out = bytes(b ^ key[((i // len(key)) + i) % len(key)] for i, b in enumerate(data))
```

Symmetric, no checksum or header. Decode as **Latin-1**, not UTF-8, or the round trip corrupts.
Format credit: [Gabarsolon/MGS4-Clarity-Fix](https://github.com/Gabarsolon/MGS4-Clarity-Fix).

Two things this established:

**The config route cannot raise internal resolution.** Setting `bufferSizeX/Y = 7680/4320` in
`mgs4.ecf` is parsed correctly — the render size monitor shows the values arrive — and then the
engine **overwrites `bufferSize` with `windowSize`** during init:

```
bufferSize=1920x1080  windowSize=1920x1080   defaults
bufferSize=7680x4320  windowSize=3840x2160   config parsed
bufferSize=3840x2160  windowSize=3840x2160   engine overwrites
```

This happens with `dynamicResolution` both true and false, so DR is not the cause. The mod's hook
works precisely because it writes *after* that clamp. Anyone documenting `bufferSizeX` as a
user-adjustable way to supersample is mistaken.

**`mgs4.dev.ecf` cannot be used as a config file, but the features behind it are alive.**
It ships with a `[debug]` block, a `[cheat]` block and a `[fastLoad]` stage jump. The key strings
`fastLoad`, `cheat`, `invincible` and `skipDemoS01a40l` are **not present in the image**, and the
two copies of `stage` have **zero references** — verified against a `render.bufferSizeX` control
in the same run. So editing that file does nothing.

> **Corrected.** The conclusion originally drawn from this - "the readers were compiled out, and
> no flag will bring them back" - is wrong. Only the *config parsing* was removed. The machinery
> itself is intact and reachable by writing its globals and calling its code, as demonstrated by
> [cipherxof/MGS4-Debug](https://github.com/cipherxof/MGS4-Debug):
>
> ```cpp
> *FastLoadStageId = stage.id;   // globals found via a signature on the fast-load function
> *FastLoadMode    = 1;
> SetStageName("select");
> *StageRequestFlags |= 0x11;
> FinalizeStageRequest();
> ```
>
> That project also shows the retail build contains a complete **ImGui developer menu** - tabs for
> DynamicResolution, Performance, Message, DofAdjust and Misc, plus stage-name and difficulty
> overlays - reachable by pattern-scanning for the ImGui entry points and the UI frame/render
> functions.
>
> The lesson generalises: an absent string proves the *string* is absent, not that the feature is.
> Code survives its configuration.

---

## 6. Tooling notes

**Getting a GPU capture.** The game exits when launched by a capture tool, and D3D12 cannot be
hooked once the device exists. The fix loads PIX's `WinPixGpuCapturer.dll` from inside the ASI
before the renderer initialises, and triggers captures with a hotkey — no launcher fight and no
GUI. `pixtool` then analyses captures headlessly: `open-capture`, `save-event-list` (CSV),
`save-screenshot`, `save-resource --global-id=N`. PIX analysis requires Windows **Developer Mode**.

**Finding the reticle draws.** `save-resource --global-id=N` dumps the render target at any draw,
so bisecting successive draws and looking for the reticle's colour pinpoints exactly which draws
paint it — five draws, a ring plus four corner brackets, out of a ten-draw pass.

**Identifying a shader.** DXBC blobs carry a 16-byte checksum immediately after the magic.
Hashing what the game loads at `CreateGraphicsPipelineState` maps any draw back to a source file.

Naming conventions differ between dumps, so check before concluding a shader is missing: the
3Dmigoto-derived `mgs4Shaders` dump names files by the checksum **directly**
(`c0f6fdb2-73bd444d-069170c9-a85fc8a1.hlsl`), while another dump seen earlier used
**byte-swapped dwords** (`2e687806` → `0678682e`). Searching for the byte-swapped form first
made the second UI shader look absent when it was present all along.

**Finding the code behind a config key.** `Find String` locates an ASCII string in the loaded
image and lists every instruction referencing it. Config keys are strings, so this goes straight
to the code that reads a setting — and shows when nothing reads it at all. Always include a
string known to exist as a control: `render.bufferSizeX` is found and referenced from the config
parser at `+3BE9B`, so a run where it appears proves the scan works.

**Launching without the launcher.** Steam runs `Launcher\launcher.exe`, a Unity front-end.
Replacing it with a wrapper that reproduces the launcher's exact `CreateProcessW` call goes
straight into the game. The command line genuinely begins with `-region` (no `argv[0]`; the path
goes in `lpApplicationName`), the working directory is `MGS4\`, and there is a double space
before the `-launcherroot` value — all reproduced verbatim, since the game parses it itself.
Steam's overlay, playtime and saves are unaffected because the wrapper waits for the game and
returns its exit code. This cut the test cycle from minutes to seconds.

**Ghidra.** The dump must be auto-analysed before the decompiler is trustworthy — without it,
function signatures are guesses (a three-argument call showed as taking two parameters). Analysis
of the 32 MB dump takes about five minutes.

---

## 7. Gotchas

**Another mod's config tool rewrites its settings file** and does not preserve keys it does
not know about. While this code lived inside that mod, any undocumented or
experimental key was silently dropped the moment the tool was used, which invalidated one round
of testing before it was noticed. MGS4 SSAA and Aspect Ratio Enabler's tool keeps unknown keys, and the research keys
now live in a separate `MGS4Enabler.lab.settings` anyway (see `features/tooling.md`).

**A stray `.asi` in the game root deadlocks a config tool that sits there.** The ASI is loaded into
the tool's process, hooks `memset` in its import table, and spawns its init thread; game
detection fails there, so that thread exits without signalling, and wxWidgets' static
initialiser blocks forever on the `memset` hook. The tool appears in Task Manager with no window
and writes no log. Keep every `.asi` under `MGS4\` (or `MGS4\scripts\`) and only tools in the
game root.

**RIP-relative displacements are relative to the end of the instruction**, which is not always the
end of the displacement field — forms like `IMUL EAX, dword ptr [rip+disp32], imm32` carry an
immediate afterwards. Assuming otherwise silently skipped 8 of 31 patch sites.

**Enabling PIX capture disables the UI fix.** PIX owns the same D3D12 entry points the mod hooks,
so the mod stands down from `D3D12CreateDevice` while capturing — and that hook is what installs
`CreateGraphicsPipelineState`, which is what substitutes the shader. A capture session therefore
renders the UI as though the fix were switched off, which looks exactly like the fix having
regressed. This confounded a full round of testing: the scope appeared fixed and the crosshair
appeared broken, when in truth neither was being patched. It now logs a warning at startup. The
tell in the log is the absence of `UI shader patch: compiled for …`.

**Frame interpolation breaks captures.** With it enabled, `CaptureNextFrame` returns frames
containing nothing but a `CopyResource` and a `Present` — 14 events and no draw calls at all.
Turn it off before capturing.

**Read the log before drawing conclusions from a screenshot.** Every wrong turn in the UI work
came from inferring mechanism from a screenshot when the log already held the answer — the render
buffer dimensions, whether the patch was even active, what constants it compiled with. Three
successive models were built and discarded before the one-line log check that settled it.

**GPU utilisation percentage is measured against the current clock.** A GPU idling at 800 MHz can
report 40% while barely working, so "GPU usage unchanged" was a misleading signal early on — the
real tell was the card holding 2.8 GHz once there was genuine work.

---

## 7b. Open questions

Things worth checking, none blocking.

**Does the Windows build have a working Vulkan path?** `mgs4.ecf` documents
`api : dx11, dx12, vulkan`, but the in-game menu offers only D3D11 and D3D12, and the file ships
`api = dx11`. The Master Collection has a native Linux build, so a Vulkan backend in the shared
engine config may simply be dead weight here - the same shape as `mgs4.dev.ecf` in §5c: a real
key with no reader left. Testable in one launch by setting `api = vulkan` and seeing whether it
starts or falls back.

Worth knowing regardless of the crosshair, which it would *not* help with: the wrap happens on
the CPU before any backend is involved, so Vulkan would receive the identical corrupted vertex
data. The one real gain would be tooling - RenderDoc inspects vertex buffers better than PIX,
needs no Developer Mode, and can edit and re-run shaders live, which would have made the shader
correction far quicker to iterate. Against that, the current fix hooks
`CreateGraphicsPipelineState` and substitutes DXBC; a Vulkan port means `vkCreateGraphicsPipelines`
and SPIR-V.

**An alternative shadow hook site.** A signature circulating elsewhere anchors on a small config
getter - `sub rsp / lea rdx,[rsp+Y] / mov [rsp+Y],<default> / lea rcx,[key string] / call lookup /
mov eax,[rax] / ret`. Reportedly it works for shadows. Note that everything identifying *which*
setting it reads - the key string and the default - is wildcarded in the pattern, so it cannot be
told apart from any other int config getter by inspection. It is also on the read path, whereas
this fix scales the value where it is parsed. Resolvable by scanning for it and disassembling the
match to read the string it loads. Two outcomes: the same value at a different point (a matter of
taste), or a shadow setting not currently touched (genuinely additive).

**Is `ShadowBufferSize` a string in the executable?** The name is certainly real - it is in
`mgs4.scalability_PC.ecf` - and the hook comment describes a key-match branch, which implies the
string is in the binary too. Not independently confirmed since; `Find String` answers it.

**Does the FXAA setting win over the game's own?** The mod hooks the getter for `render.fxaa`, but
`enableFXAA` also exists in `mgs4.savedsettings` as the in-game toggle. Which takes precedence
when they disagree is untested. Check by unticking the mod's box with the game's menu setting on.

**The producer.** Still unidentified; see §5.8b. The packed-store scan leaves 86 packs and 103
wide stores with no cheap way to narrow them.

**CI releases.** The merge from upstream brought a finished `create_release.yaml` and zip
structure, which could replace packaging releases by hand.

---

## 8. Credits

The shader work in [§5.5](#55-the-fix) rests entirely on community tooling. Without the shader's
source and a way to identify it by checksum, the reticle bug had no tractable fix.

| Contribution | By |
| --- | --- |
| `.vfp` shader pak format reversed | **ermaccer** |
| Pak extraction script (`mgs4_vpak_unpacker.py`) | **VAXIStaa** |
| The extracted shader dump used here (`mgs4Shaders`) | **ICantReadYourMind** |
| DXBC → HLSL decompilation | **3Dmigoto** (v1.2.45) |

These were shared on Discord, so there is no durable link to cite. The dump itself is
reproducible from your own installation: run the extraction script against
`MGS4\shaders\vfp_PC.1.pak` / `.2.pak` to recover the DXBC blobs, then decompile those with
3Dmigoto to get readable HLSL. Only the shader identified in [§5.5](#55-the-fix) is needed to
rebuild the fix; [§6](#6-tooling-notes) covers matching a shader to a draw by checksum.
