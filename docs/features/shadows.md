# Shadows

**Settings:** `Shadow Resolution Scale (%)`, `Shadow Softness (Samples)`
**Implements:** `asi/src/features/graphics_settings.cpp`, `ApplyShadowAndAntiAliasing`

## What it does

Raises the shadow map resolution beyond the game's highest Shadow Quality preset, and separately
raises the number of samples taken when filtering a shadow edge. Resolution is detail; samples
are edge smoothness. They are independent and useful separately.

## What it patches

Both come from the engine's scalability config, parsed and stored before anything is allocated,
so scaling the value at parse time raises the shadow map everywhere the engine derives from it.

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
**only** by the group id written into the same slot — 4 rather than 5. That discriminator is the
entire reason the two hooks do not collide, so it is load-bearing rather than incidental.

| Hook | Matched at | Group |
| --- | --- | --- |
| Shadow resolution | `+0x142BDA` | 5 |
| Shadow samples | `+0x142B39` | 4 |

The atlas is allocated as width x 2*width, so a scale raises both dimensions: at 400% a 2048
buffer becomes 8192, and the atlas goes from 2048x4096 to 8192x16384.

The game's highest preset supplies 7 samples.

## Limits

None enforced beyond the setting ranges. Shadow memory grows with the square of the resolution
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
