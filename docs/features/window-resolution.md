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

### WR-002 — windowed mode with the in-game resolution below the override: black on D3D11, misaligned on D3D12

**Reported** by the user 2026-09-07: windowed mode, 2560x1440 chosen in the game's own options,
the override at 3200x1800 on a 3840x2160 desktop. D3D11 showed nothing, D3D12 drew the picture
offset. Setting the in-game resolution at or above the override was fine.

**Cause (Measured).** The engine keeps its resolution twice. The window-size globals, which the
override writes at config time, size the window (in windowed mode the window's client area was
our 3200x1800) and the engine's own render targets and viewports. The renderer context keeps a
second copy in its init record (`context+0x4C1230C/+0x4C12310`), filled by the game's own
resolution setting, which the per-frame submit at `+76DFF0` copies into each frame
(`movups [frame+0x24D35C8], xmm0` at `+76E01D`, found with a displacement search over the
whole executable). The backends size their own buffers from that copy, clamp view rects to it
and ask for the swap chain resize with it: a lab caller walk from `ResizeBuffers1` went
`+7E2078 < +7A9767 < +7A5779 < +76628F`, the D3D12 backend's `updateResolution` reading the
frame record. So the window and the engine's targets were 3200x1800 while the renderer's
buffers and clamp were 2560x1440: D3D11 drops draws whose colour and depth targets differ in
size (black), D3D12 clamps the view rects (misaligned). With the in-game value at or above
the override the clamp never bites, which is why fullscreen and the earlier tests passed.
(This also re-reads AR-014: what the engine "asks for" at the fullscreen resize is this copy,
i.e. the game's own resolution setting, which defaults to the largest display mode.)

**Fix.** `InstallRendererResolution` (render_pipeline.cpp) hooks the submit at `+76DFF0`
(prologue verified) and, with the override on, writes the **game window's client area** into
the init record's width and height before it runs, so every frame's resolution is the
surface the chain is sized to: the override in windowed mode, the desktop in fullscreen. A
first version wrote the override itself; that was right in windowed mode and wrong in
fullscreen-windowed, where the chain is the desktop and the renderer's buffers came out
smaller than it - a black screen on D3D11 again (user, same day). The renderer's resolution
must never be smaller than the chain; equal is the rule. In windowed mode the chain request
then equals the window and the private module's `SizeToWindow` has nothing to substitute; in
fullscreen the value equals the game's own (the desktop) and the fit places the picture as
before. Log lines: `renderer resolution hook installed`, then `the renderer's own resolution
2560x1440 -> 3200x1800 (the window's client area, which is the chain)` when it differs, and
`ResizeBuffers1(3200x1800) requested` in a lab build. Verified: D3D12 windowed, override
3200x1800 with 2560x1440 chosen in-game, window client 3200x1800 on the 3840x2160 desktop,
title scene filling it; D3D11 windowed, in-game resolution changed while running, fine
(user); D3D11 fullscreen-windowed on the 3840x2160 desktop with the override at 3200x1800,
main menu filling the screen from a desktop grab (the harness's window capture is black for a
windowed flip-model chain, a capture limitation, not the game).

The window itself is recorded from the swap chain (the public D3D11 creation hook and the
private module's chain hooks set `RenderPipeline::hGameWindow`), never found by enumeration:
the first 0.0.4 build looked it up with `EnumWindows` + `GetWindowThreadProcessId`, and the Nexus
upload scanner flagged that zip as unsafe while 0.0.3 passed - walking other processes' windows
by id is an injector's signature. With the enumeration gone the import table gained only
`GetClientRect` and `GetSystemMetrics`. Until the window is known the renderer's own value
stands; writing the desktop size in that gap cost one spurious chain resize at boot.

Alongside it, the render-config hook now caps the override to the surface it will land in
(the game window's client area once it exists, the primary desktop before that), shrinking
it with its shape kept and logging `the AxB override is larger than the CxD window; using ExF`.
That is a separate, lesser problem, an override larger than the screen, and not the cause of
this bug.

---

## Retracted / unverified

None recorded.
