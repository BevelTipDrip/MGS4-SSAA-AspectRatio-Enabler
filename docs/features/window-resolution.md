# Window resolution

**Settings:** `Window Aspect Ratio`, `Window Resolution (16:9)`, `Window Resolution (21:9)`,
`Window Resolution (32:9)`, `Window Resolution (4:3)`
**Implements:** `asi/src/features/render_pipeline.cpp`, `asi/src/core/config.cpp`

## What it does

Overrides the resolution the game runs its window and swapchain at — its actual output size,
independent of the internal render buffer.

## What it patches

The same config init as supersampling, writing `render.windowSizeX/Y` at `+0x1B00460` and
`+0x1B00464` rather than the buffer size.

The aspect ratio setting selects **which resolution list applies**, and is not itself a scaling
mode:

| `Window Aspect Ratio` | Effect |
| --- | --- |
| `Use Game Setting` | no override at all; the resolution keys are not even read |
| `16:9` | reads `Window Resolution (16:9)` |
| `21:9` | reads `Window Resolution (21:9)` — 2560x1080, 3440x1440, 3840x1600, 3840x1620, 5120x2160 |
| `32:9` | reads `Window Resolution (32:9)` — 3840x1080, 5120x1440, 7680x2160 |
| `4:3` | reads `Window Resolution (4:3)` |

The aspect names are labels for the lists, nothing more: the ASI never checks that the chosen
resolution matches the aspect, and the 21:9 list deliberately includes the 43:18 and 12:5 panel
sizes sold as "21:9".

**This trips people up, including me.** With the aspect left at `Use Game Setting`, a resolution
sitting in the settings file is ignored entirely and the game runs at the display resolution. A
whole set of measurements in this project were mislabelled because of it — the settings file said
`1920x1080` while the game was rendering at 4K, and the resulting arithmetic was self-consistent
but wrong. **Read the render size from the log, never from the settings file:**

```
MGS4: Internal Resolution: render buffer 3840x2160 -> 7680x4320 (2.00x scale). Output stays at 3840x2160.
```

---

## Active bugs

None recorded.

---

## Fixed

### WR-001 — a 4:3 window squashes the interface horizontally

- **Status:** Fixed, 2026-09-04
- **Evidence:** Observed — lab captures at 2880x2160, read by the user
- **Affects:** `Window Aspect Ratio = 4:3` with any of its resolutions, `Ultrawide HUD =
  Stretched HUD`
- **Repro:** set the aspect to 4:3, pick a resolution, and compare HUD proportions against 16:9
- **Evidence detail:** originally recorded in the shipped README as a known issue with no
  measurement attached. Reproduced and fixed in the aspect-ratio work
- **Popups:** none
- **Cause:** the game's UI is authored for 16:9 and is fitted to the window with no correction
- **Fix:** the `Ultrawide HUD` setting (Centered or Expanded HUD) handles a taller-than-16:9
  window the same way it handles a wider one. It stays filed here because this is the setting
  that exposes it; the fix belongs to the aspect-ratio feature

---

## Retracted / unverified

None recorded.
