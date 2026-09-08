# Shadows

**Settings:** `Shadow Resolution Scale (%)`
**Implements:** `asi/src/features/graphics_settings.cpp`, `ApplyShadowAndAntiAliasing`

## What it does

Raises the shadow map resolution beyond the game's highest Shadow Quality preset.

## What it patches

The size comes from the engine's scalability config, parsed and stored before anything is
allocated, so scaling the value at parse time raises the shadow map everywhere the engine
derives from it.

`ShadowBufferSize` is group 5:

```
TEST DIL,DIL              ; did the key match "ShadowBufferSize"?
JZ   <skip>
LEA  RCX,[RBX + 0x30]
CALL <parse int>          ; -> EAX
MOV  [RBP-0x60], 5        ; group 5, and where we hook
MOV  [RBP-0x50], EAX      ; the value gets stored here
```

`ShadowSampleCount` is the branch immediately above, structurally identical and distinguished
**only** by the group id written into the same slot — 4 rather than 5. That discriminator is
what keeps the hook off the wrong branch, so it is load-bearing rather than incidental.

| Hook | Matched at | Group |
| --- | --- | --- |
| Shadow resolution | `+0x142BDA` | 5 |

The atlas is allocated as width x 2*width, so a scale raises both dimensions: at 400% a 2048
buffer becomes 8192, and the atlas goes from 2048x4096 to 8192x16384. The hook fires once per
quality tier (512, 1024, 2048 at the game's presets).

## Filtering: why the sample-count setting was withdrawn (2026-09-07)

0.0.1 to 0.0.5 shipped `Shadow Softness (Samples)`, a hook on the group-4 branch that raised
the parsed `ShadowSampleCount`. The user found it never softened anything while FusionFix's
`ShadowTexelOverride` did, and the shader and the engine code (read from a live dump of
`.text`, which is encrypted on disk) show why:

- The shadow pixel shaders (fxc disassembly of the DirectX 11 original
  `00acb728-47adeb51-a7091a37-42cd3834`) filter with a centre tap and a ring of `count - 1`
  taps, `count` from `vts_shadowSampleCount` (cb0[30].x), at a radius of
  `0.7 * vts_shadowbufferSize.xy` (cb0[29]), normalised by `1 / count`. More taps make the
  ring denser; the radius is the other uniform.
- The engine sets that uniform to one texel of the atlas: the setter at `+663FA0` is called
  with `(1/W, 1/(2W))` for pass 0 and `(1/W, 1/W)` for pass 1, `W` being `ShadowBufferSize`
  (callers at `+EEA60` and `+F467F`; 214 of the DirectX 11 pixel shaders read `.xy`, 102
  read `.zw`). So the edge is never wider than a texel, and a higher Shadow Resolution Scale
  makes it *harder* and more aliased, not smoother.
- FusionFix's `ShadowTexelOverride` is that texel as a decimal fraction of the map
  (`0.001953125` = 1/512): its hook on the setter's entry writes the value into `XMM1`
  and half of it into `XMM2`. Its replacement shaders are a fixed 8-tap Vogel spiral over the
  same radius, and a second hook on the uniform upload (`+66C48E`) redirects the sample
  count to a constant 8, whatever the game (or a hook on the parser) supplied. FusionFix
  installs both hooks whenever it is loaded, including with the texel set to 0, which it then
  writes as is (a filter of no width) rather than restoring the game's own texel.

The setting was therefore removed rather than re-expressed: the width is FusionFix's
setting, on its tab, and the count is pinned by FusionFix for its own shaders. A native
width override was prototyped (hook the setter's compare at `+663FAD`, write the pair into
the uniform table and force the upload flag, which sits in the dword before the table) and
dropped at the user's call.

## Limits

None enforced beyond the setting range. Shadow memory grows with the square of the resolution
scale, and the atlas is already double-height, so large values are expensive quickly.

---

## Active bugs

None recorded.

---

## Fixed

None recorded.

---

## Retracted / unverified

### SH-001 — interaction between shadow scaling and internal resolution scaling is uncharacterised

- **Status:** Unverified — not known to be a bug
- **Evidence:** Inferred
- **Affects:** potentially any combination of a high shadow scale and a high internal resolution
- **Evidence detail:** none. Both settings have been run together at high values (400% shadows
  with a 7680-wide buffer) during testing with no visible problem, but nobody has looked for
  interaction specifically, and no VRAM ceiling has been derived for the combination
- **Cause:** n/a
- **Why it is listed:** so the gap is visible rather than mistaken for a clean bill of health.
  Delete this entry if it is ever characterised either way
