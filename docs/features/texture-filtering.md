# Texture filtering

**Setting:** `Anisotropic Filtering` — 0 (off) to 16
**Implements:** `asi/src/features/render_pipeline.cpp`, `Hooked_CreateSampler`

## What it does

Raises the maximum anisotropy on textures viewed at a steep angle — floors, walls and terrain
seen edge-on. The game's highest texture preset asks for 8x; 16x is the hardware maximum and
close to free on modern GPUs.

## What it patches

Unlike the other graphics settings, this one is not a patch to `mgs4.exe` at all. It hooks
`ID3D12Device::CreateSampler` (vtable slot 22) and raises `MaxAnisotropy` on samplers that
**already request anisotropic filtering**.

That condition matters. Samplers asking for point or linear filtering are left alone, because
raising anisotropy on them would change filtering the game deliberately chose — a UI element or
a lookup table sampled with a point filter is not asking for a quality improvement.

Because it rides the D3D12 device hook, it depends on that hook being installed, which is also
what the UI research uses. It is disabled while PIX capture is active, since PIX owns the D3D12
entry points.

## Limits

Clamped to 16, the hardware maximum.

---

## Active bugs

None recorded.

---

## Fixed

None recorded.

---

## Retracted / unverified

None recorded.
