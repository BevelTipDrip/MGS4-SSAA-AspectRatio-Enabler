# Supersampling

**Setting:** `Internal Resolution Scale (%)` — 100 to 400, default 100
**Implements:** `asi/src/features/render_pipeline.cpp`

## What it does

Renders the game internally at a multiple of the output resolution and scales it down on
presentation. The window and swapchain keep the game's configured size, so it works on a monitor
that cannot exceed its native resolution.

## What it patches

The engine parses four render config keys into fixed globals at the start of `.data`:

| Key | Global | Meaning |
| --- | --- | --- |
| `render.bufferSizeX` | `+0x1B00000` | internal render buffer width |
| `render.bufferSizeY` | `+0x1B00004` | internal render buffer height |
| `render.windowSizeX` | `+0x1B00460` | window / swapchain width |
| `render.windowSizeY` | `+0x1B00464` | window / swapchain height |

`bufferSize` is genuinely separate from `windowSize` — the engine's own DynamicResolution debug
panel calls it "Buffer". The fix hooks the config init (matched at `+0x3C09A`) and multiplies the
buffer size after parsing, leaving the window size alone.

## Limits

| Limit | Value | Reason |
| --- | --- | --- |
| Scale | 400% | Deliberate. Cost grows with the square of the setting |
| Buffer width | 8192 | Deliberate 8K ceiling. Both axes scale together, so aspect is preserved |

Neither is a technical boundary. The former 4092 and 6720 ceilings were, and are gone — see
SS-001.

---

## Active bugs

### SS-002 — the whole frame is downscaled, so the HUD shrinks at high scales

- **Status:** Active
- **Evidence:** Observed
- **Affects:** any scale above 100%, more noticeable the higher it goes
- **Repro:** set the scale to 200% or more and compare HUD text against 100%
- **Evidence detail:** inherent to the approach — the fix raises the whole render buffer, and
  presentation scales the entire frame down, UI included
- **Screenshots:** none captured. A 100% and 400% pair of the same HUD would demonstrate it
- **Popups:** none
- **Cause:** known. Supersampling does not separate world rendering from UI composition, so the
  interface is supersampled and shrunk along with everything else
- **Fix:** none. Would require compositing the UI after the downscale, at output resolution

---

## Fixed

### SS-003 — the sniper scope breaks when looking down and to the right

- **Status:** Fixed in 0.2.4, by removing its cause
- **Evidence:** Observed
- **Affects:** scoped sniper rifles at a raised internal resolution, **with the shader
  workaround enabled**
- **Repro:** equip a sniper rifle, zoom, and look down and to the right
- **Evidence detail:** reported by the user during development, verbatim: *"when zooming in with
  sniper rifles, the scope breaks when looking down and to the right"*. Root cause confirmed by
  the user as the wrap-around shader workaround
- **Screenshots:** none retained
- **Popups:** none
- **Cause:** the shader workaround had to *infer* which coordinates had wrapped, since the
  original value was gone by the time the shader ran. The scope vignette is anchored at a screen
  corner by construction, which puts it exactly on that inference's detection boundary - so the
  correction fired on a coordinate that had not wrapped. **The wrap itself never affected the
  scope**; the workaround did
- **Fix:** none targeted. The workaround was removed outright in 0.2.4 once the truncation was
  fixed at its source, which removes the cause
- **Verified:** the user reports several hours of ordinary play at a raised internal resolution
  on 0.2.4 with no issues
- **See also:** SS-004, the other artefact the same workaround produced

### SS-001 — the aiming reticle disappears above a 4095-wide render buffer

- **Status:** Fixed in 0.2.4
- **Evidence:** Measured
- **Affects:** any internal buffer wider than 4095 px, independent of window size
- **Repro:** aim with any weapon at an internal buffer above 4095 wide; the reticle is absent
- **Evidence detail:** three independent confirmations.
  1. PIX capture at 7680x4320: reticle vertices leave the vertex shader at NDC `(-1.07, +1.90)`,
     i.e. screen pixel `(-256, -1936)`.
  2. The NDC anchor read live from the vertex stream at runtime: `v8 = (-1.0666, 1.8962)` at a
     7680-wide buffer, against `(0.0, 0.0002)` at 3840. Predicted by int16 wraparound to four
     decimal places on both axes.
  3. Window-independence: a 3840-wide buffer renders the reticle behind both a 4K and a 1080p
     window, while 7680 fails behind both.
- **Screenshots:** the same scene and pose, differing only in buffer width and the fix:
  - [`SS-001-reticle-absent-7680x4320.jpg`](evidence/SS-001-reticle-absent-7680x4320.jpg) —
    7680 wide, unpatched. Aiming, HUD intact, no reticle
  - [`SS-001-reticle-present-3840x2160-control.jpg`](evidence/SS-001-reticle-present-3840x2160-control.jpg) —
    3840 wide, unpatched. The control: same everything, reticle present
  - [`SS-001-reticle-present-after-fix-7680x4320.jpg`](evidence/SS-001-reticle-present-after-fix-7680x4320.jpg) —
    7680 wide with the fix. Reticle present at a width that never worked before
- **Popups:** none. The failure is silent, which is the whole difficulty — an integer overflow
  produces no error and the draw calls still issue with a correct viewport and scissor
- **Cause:** the engine converts the reticle's screen position into its 1280x720 virtual UI
  space and kept only the sign-extended low 16 bits of a value it already held at full precision
  (`movsx edx, cx`). At 1/16px units that caps a coordinate at 2047.94px, and the reticle sits at
  `bufferWidth / 2`
- **Fix:** four narrowing conversions widened to full-width moves, located by shape rather than
  address. `asi/src/features/graphics_settings.cpp`, `PatchReticleTruncation`
- **Verified:** reticle renders at 8192x4608 with the shader workaround removed; several hours
  of ordinary play at a raised internal resolution with no interface problems. A read-only scan
  found 24 narrowings in the binary of which exactly these 4 are scaled by a render buffer size
- **Credit:** [drbermejor/mgs4Ultra120](https://github.com/drbermejor/mgs4Ultra120) identified
  that these conversions were the cause

---

## Retracted / unverified

### SS-004 — "some menu text breaks" at high internal resolution

- **Status:** Retracted
- **Evidence:** Retracted — never had any
- **Original claim:** *"Above roughly a 4K-wide render buffer, the aiming reticle disappears
  completely. Some menu text also breaks."*
- **Why retracted:** it entered the research notes in their first commit and was repeated as
  fact afterwards with no measurement, capture or reproduction behind it, unlike the reticle in
  the same sentence. It was then recorded as fixed by the truncation patch — asserting a fix for
  a bug never confirmed to exist
- **Contradicting observation:** the user reports broken menu text was only ever seen **with the
  shader workaround enabled**. That fits the mechanism: the workaround inferred which coordinates
  had wrapped, and its edge margin could misfire on text legitimately near a screen edge. On that
  reading it was an artefact the workaround created, and removing the workaround removes it
- **See also:** SS-003, a confirmed bug with the same cause - the workaround misfiring on
  an element legitimately near a screen edge. That SS-003 is confirmed makes this
 mechanism real rather than hypothetical, though it still does not evidence this claim
- **If it recurs:** file a new ID with a screenshot, the exact resolution and the settings in
  force. Do not reopen this one
