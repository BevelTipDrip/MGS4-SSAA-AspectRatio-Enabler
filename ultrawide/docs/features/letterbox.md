# Letterbox: cutscenes and menus kept at 16:9 (AR-010)

**Status:** shipping in 0.2.6 (2026-09-04) as three switches - `Menu Masking`,
`Cutscene Masking`, `Cutscene FOV Compensation` - plus the gameplay `FOV Adjustment (%)`
that reuses the camera hook (section 6). Lab settings keep `Aspect Letterbox=1` as a
shorthand that sets all three masks, and the shipping keys read on top of it. Code: the
"Letterbox" section of `src/fixes/aspect_ratio.cpp` and the screen table just above
`ShiftAppend`; the switch plumbing is in the public `config.cpp`.

## 1. What it does

At any output that is not 16:9, cutscenes, the codec, the pause menu and the title/main menu
are shown as the 16:9 picture centred in the window, with black bands over the rest - side
pillars at 21:9, top and bottom bands at 4:3. Gameplay is untouched: the HUD spread and the
world keep the full window. The user asked for this explicitly (cutscenes framed as authored;
"doing it natively in the game may be a more robust option").

Two pieces:

- **Camera.** The engine's cutscene camera at 21:9 is Vert-: the same horizontal extent as
  16:9 with the top and bottom cut off (AR-002, measured on s01a10l_D2's Snake close-up).
  The cinematic camera builder at `+0x0B9BB0` takes a projection scale; dividing it by the
  horizontal spread factor (W / (H·16/9), 1.333 at 3840x1620) is a uniform zoom-out that puts
  the 16:9 frame back inside the wide window. Only cinematic routes are touched: the call from
  `+0x0B9BA0` while the cinematic owner `+0x652E00` is on the stack (a thread-local depth
  counter), and the final rebuild from `+0x0EB0EB` recognised as Ultra120 does it - same
  camera object, same input scale, within 250 ms of the tagged call. The gameplay route
  (`+0x0BA3A3`) passes through. At 4:3 the factor is 1.0 and the camera is left alone: the
  stock 4:3 frame already contains the 16:9 picture.
- **Bands.** Two black rects cleared onto the back buffer with `ClearRenderTargetView` right
  before `Present` (DXGI factory → swap chain vtable hooks; `Present`, `Present1`,
  `ResizeBuffers`). The rects are computed per Present from the engine's window-size globals,
  not from the swap chain: the swap chain is desktop-sized (3840x2160 on this machine) and a
  3840x1620 window uses its top rows, a 2880x2160 window its left columns, so the bands are
  placed inside that region. (The "Factor 1.0000" in the install log is logged before the
  window globals are populated; harmless.)

Measured at 3840x1620 (`projlog`): m00 2.41421 → 1.81066 on the cinematic route, m11 held at
4.29193 = the 16:9 stock value, m11/m00 = 2.3704 = the window's ratio. At 2880x2160: 0
patches, bands 270 px top and bottom.

## 2. The gate - when the bands are on

`BandsWanted()` (live `box 0|1|2`: gated, forced on, forced off). Gated means, per switch:

    (bMenuMasking     && (BootPhaseLive() || ScreenLive()))
 || (bCutsceneMasking && CutsceneCameraLive() && !PostLoadGrace() && !plateUp)

with the boot ladder stepped (`BootPhaseLive()`) *before* the grace is read - the order
matters on the dismissal present, where the ladder goes Over and opens the grace in the same
call (split_b painted one present the other way round). The two signals, each live for
150 ms after its last sighting:

- **Cutscene:** a cinematic camera was built (`g_CinematicBuiltTick`, set in the builder hook
  above). Structural - no state flag, no stage name. The title screen's graveyard is a
  cinematic camera too, so the title is covered twice.
- **Screen:** an element from `kScreenIdentities` was appended to the draw pool
  (`g_ScreenBuiltTick`, set in `ShiftAppend` from the same `IdentifyAppend` unwind the HUD
  spread uses - node strcode at frame-2 `rbx`+8). The table, from the 2026-09-04 census:

  | strcode | screen | where it draws |
  | --- | --- | --- |
  | `005F9476` | pause menu / codec | heap owner, channel 2, la2 object 001997E2 |
  | `001AAC76` | pause menu / codec | heap owners, channels 1 and 3, same object |
  | `00042FAA` | title PRESS ANY BUTTON | HUD owner, channel 1 (parked above the screen on channel 0xB while the main menu list is up) |
  | `0052133A` | main menu list | HUD owner, channel 0xA, 14 records (the rows and the MENU bar) |
  | `004FC96A` | fade plate | HUD owner, channel 3 |

  Each was present in every dump of its screen and absent from gameplay and the BASIC
  CONTROLS overlay. `0060CD29` (pause menu) is *not* tabled: it also fires on gameplay
  channel 0x2A. The fade plate is tabled because it is 16:9-sized - at 21:9 the world shows
  past its sides while it is up (title boot capture) - and bands under a black plate cost
  nothing elsewhere.

The strcode read needed one change for this: the menus' heap owners execute their layout
nodes from outside the script block `r14` points at (node − r14 = 0xA48630 on the pause
menu), and `IdentifyAppend` only read nodes inside it. It now reads an out-of-block node
after a `VirtualQuery` page check. The HUD tables are unaffected (in-block nodes, and the
treatment is still keyed on the HUD owner).

### 2.1 The boot phase - bands from launch to the continue press

The user's directive (2026-09-04): "the mask can be applied all the way up until a user
loads a save, and hits the continue button once the game loads", and "we can remove the
black bars as soon as the user presses a button to continue from the loading screen, no
reason to have them up all the way until the camera snaps to the player... it looks
awkward." So a third signal, `BootPhaseLive()`, holds the bands from launch through the
logos, the title, the menus and the load plates, and drops them at the key press on the
loaded-save plate (Outer Haven skull + PRESS ANY BUTTON).

The pool is a **change stream, not a per-frame draw list**: a static screen appends nothing
(the skull plate is silent for 20+ s while it waits), so "is the plate live" cannot be read
from a node timeout - the first ladder did that and went Over 170 ms after the plate came
up (spin219b). The ladder is therefore keyed on two one-shot identities, both HUD-owner
appends read by the same unwind as the HUD spread:

| event | identity | when |
| --- | --- | --- |
| plate up | ch 1, elem 0x105F0 (op 13 operand 1) and 0x10710 (op 13 operand 0), no strcode | as the skull fades in |
| plate dismissed | ch 38 elem 2C8 strcode `0071FC3E` and ch 37 elem 248 strcode `0008F2D7` (op 0D) | ~0.6 s after the key |

Identical in every run (boot43a/b/c, load43g, spin219a-e, spin43a; 21:9 and 4:3; the
rusted-barrel save only). A user's own key press names the same pair (spin219e: the user
pressed continue while watching, dismissal logged 3.7 s after the plate).

`Armed → PlateUp → Over`; `RearmBootPhase()` on the title's `00042FAA` (a return to the
title re-arms it). Fallbacks if an identity never comes: Armed goes Over after 15 s of the
gameplay camera alone (no cinematic, no screen - covers NEW GAME, which has no plate),
PlateUp after 3 s of the same (another save naming other dismissal channels).

**Post-load grace.** After the press the level runs its intro pan through the cinematic
camera route for ~3.5-4 s, so `CutsceneCameraLive()` alone kept the bands up until the
camera reached Snake (spin219c) - exactly the "awkward" the directive names. The dismissal
opens a grace (`PostLoadGrace()`, 10 s cap) during which the cutscene signal is muted; it
ends as soon as that camera has been seen and goes quiet (`post-load grace over (intro
camera done)`, 3.6 s after the dismissal in spin219e and spin43a). It is evaluated every
present, not inside the cutscene short-circuit - the first version was, and never ended
before its cap (spin219d). Real cutscenes later get bands as before.

### 2.2 The loading spinner

The spinner at the top right of the load plates is its own object, not a layout node: a
48-byte record appended twice a frame from `+B5830` / `+B58E7` (frame-1 rbp 0, frame-1 rsi
= module + `0x23CF7E88`, a static object - the identity), floats at +16..+31 = x 0.9068,
y 0.8815, w 0.05, h 0.0889 as fractions of the **window** (a 64x64 px sprite in 1280x720
terms). Because it is window-normalised it stretched with the window (183x132 px at
3840x1620) and sat in the 21:9 corner outside the 16:9 frame. `LoadIconAppend` maps it
into the centred 16:9 frame: x' = (1 − 1/fx)/2 + x/fx, w' = w/fx, and the same with fy on
y/h (exactly one factor is > 1). Logged once per boot:

    21:9  (0.9068,0.8815 0.0500x0.0889) -> (0.8051,0.8815 0.0375x0.0889) (factors 1.3333/1.0000)
    4:3   (0.9068,0.8815 0.0500x0.0889) -> (0.9068,0.7861 0.0500x0.0667) (factors 1.0000/1.3333)

The same object draws the spinner on the initial boot load (the line is logged during the
logos), so both loads are covered.

## 3. Evidence

All lab captures, 21:9 = 3840x1620, 4:3 = 2880x2160; the 21:9 cutscene framing is
user-confirmed ("the framing looks correct, or at least extremely close"), the rest are my
read pending the user's:

- [cutscene, 21:9](evidence/AR-010-cutscene-21-9.jpg) - s01a10l_1 opening, t=15..90 s
- [cutscene, 4:3](evidence/AR-010-cutscene-4-3.jpg) - same, bands only, no zoom
- [pause / codec, 21:9](evidence/AR-010-pause-codec-21-9.jpg) - gameplay (no bands), pause
  with NOTICE, pause, codec, back to gameplay (no bands), pause again
- [title, 21:9](evidence/AR-010-title-21-9.jpg) - fade plate (captured before the gate saw
  it, world visible at the sides), PRESS ANY BUTTON, main menu list
- [pause, 4:3](evidence/AR-010-pause-4-3.jpg)
- [load path, 21:9](evidence/AR-010-load-21-9.jpg) - spin219d: load plate with the spinner
  inside the frame, the skull plate, then continue01/02/03/05 (captures at 4 Hz from the
  Enter) - bands gone at continue01, world full width with the HUD in the 21:9 corners.
  User-confirmed ("spin219d looks correct, and I saw the whole process", 2026-09-04)
- [load path, 4:3](evidence/AR-010-load-4-3.jpg) - spin43a: same sequence at 2880x2160,
  spinner mapped on y/h. User-confirmed ("Looks correct", 2026-09-04)

Log lines to look for: `letterbox: screen element XXXXXXXX (...) seen; bands follow it.`
(once per identity per boot), `boot phase: loaded-save plate up.`, `boot phase over (...)`,
`post-load grace over (...)`, `UI Aspect: loading spinner rect (...) -> (...)`, and
`boxstat`'s `boot armed|plate-up|over ... plate-seen N ms ago cutscene-live yes|no
screen-live yes|no (last XXXXXXXX)`. `cleared` in `boxstat` counts presents that painted
bands; it freezing after the dismissal is the "no bands from the press on" check.

## 4. Lab

- `cutscene_run.ps1 -LiveAt "sec:command"` - boots a stage's opening cutscene (rewrites
  `Stage Automation Stage` in the lab settings), fires live commands at given seconds,
  captures on a schedule, closes the game.
- `state_census.ps1 -Phase gameplay|codec|title|load` - boots (the barrel save, or to the
  title), walks the screen states with `send_input.ps1` (Tab = pause menu, Enter = confirm,
  RClick = UI cancel, Esc = BASIC CONTROLS), and at each state writes `boxstat` + `sites` and
  captures. Copies the game log next to the captures at the end (the log is fresh per boot).
  `-Phase load` walks LOAD GAME → save list → confirm → 16 polls over the load plates →
  the skull plate → Enter → 8 captures at 4 Hz → gameplay; `-SitesAt n,..` adds a sites dump
  at those polls, `-DrawAt n,.. -DrawCount N` a command-list draw census. It arms
  `screenlog 500 all` at the confirm (the loading-done burst alone is ~95 appends).
  Needs the game in the foreground - a run with the user at the desktop captured the
  desktop over the game (spin219e).
- `sites_census.ps1 [-LogPath] [-Against <dump>] [-Full]` - tabulates each `sites` dump by
  identity (owner, channel, node strcode, la2 object) with counts and x/y ranges; `-Against`
  prints only what a dump has that the reference does not. Dump 0 is usually empty (armed
  before any append), compare against 1+.
- `sheet.ps1 -Dir <captures> -Names a,b,c -Crop 'W,H'` - contact sheet of a run's captures,
  cropped to the window (the captures are desktop-sized).
- Live: `box`, `cutzoom 0|1`, `cutdiv <f>`, `camlog N`, `projlog N`, `boxstat`,
  `screenlog N [all]` (log the next N attributed appends - strcode nodes only, or every
  append with `all`; each identity once per boot), `sites` (attributed dump of the next pool
  upload, including out-of-block records as `rec out@`), `drawlog N` (the next N
  DrawInstanced / DrawIndexedInstanced calls with their return sites).

Timing caveat: the codec phase's key sequence assumes the save is up 3 s after the harness
returns; at 4:3 the load ran longer and the Tab landed on the load screen, so the pause menu
only appeared in the last capture. Poll for the HUD before pressing keys if this recurs.

## 5. Not done

- **Codec / pause menu at 4:3** has one capture (the pause menu); the codec itself was not
  reached in that run.
- **Submenus** (LOAD GAME list, OPTIONS, Drebin's shop, briefing) were not censused. The
  title ones are covered by the cutscene signal anyway; in-game ones fall back to no bands -
  a cosmetic gap, never a wrong jump. Each is one `sites` dump and one table line.
- **The band edge on cutscene transitions**: the 150 ms window means a frame or two of full
  window at the start and end of a cutscene. Not visible in the captures; not measured.
- **Pre-rendered videos** (the D1 opening) are not cinematic cameras; untested.
- **Boot phase on other paths**: NEW GAME (no plate; relies on the 15 s camera-alone
  fallback) and saves other than the rusted barrel (the dismissal channels 37/38 may be
  save-specific; the 3 s PlateUp fallback covers it) have not been run.
- **The cutscene-only load path** (menu masking off) leaves the title, main menu and load
  plates unmasked but still masks the title graveyard, which is a cinematic camera. Consistent
  with the switch's name; not a bug, noted so nobody chases it.

## 6. Shipping switches (0.2.6)

The lab's single `Aspect Letterbox` became four settings in `[Graphics]`, all read in the
public `config.cpp` and exposed in the Config Tool:

| setting | default | what it drives |
| --- | --- | --- |
| `Menu Masking` | on | the `ScreenLive()` and boot-phase terms of the gate; forced off when `Fix UI Aspect` is off (Stretched HUD), since the screen identities come from the pool hook, and the Config Tool greys it out unless the HUD is Centered/Expanded |
| `Cutscene Masking` | on | the `CutsceneCameraLive()` term |
| `Cutscene FOV Compensation` | on | seeds `g_CutsceneZoom` (the cinematic-route division by the spread factor); a no-op at 4:3 and 16:9 |
| `FOV Adjustment (%)` | 100 (50-200) | `fFovAdjustment`; the gameplay route's projection scale is divided by it |

A 16:9 window (`Window Aspect Ratio` explicit 16:9) skips all four - `bSixteenNineWindow`
in `config.cpp`. `CameraHooksWanted()` / `BandHooksWanted()` decide which hooks install:
the camera builder hook when any of the four is active, the DXGI hooks only for the two
masks. The pool-append probe now carries `screens` (either mask) and `icon` (spinner,
keyed on `bFixUiAspect`) as separate flags, so cutscene-only masking works without the
HUD spread.

**Plate-up mute.** With menu masking off the split run (split_a) painted seven presents at
the continue press: the level's intro pan is a cinematic camera and it is live *before* the
dismissal identities open the grace. The cutscene term is therefore muted while the ladder
is at PlateUp as well as during the grace. Verified in split_c: `painted` and `cleared`
flat from the plate through the press and the grace end.

**Gameplay FOV.** The gameplay camera is rebuilt several times per frame: first from
`+0x0B9BA0` at depth 0 (before the `+0x0BA3A3` call the hook recognises), then from
`+0x0EB0EB`, `+0x0EB191`, `+0x0B8A43`, `+0x0B8B4F`. The later rebuilds read the scale
back from the camera object, so they already carry the divided value (fov130 log:
`1.38462 -> 1.38462`); only the `+0x0B9BA0` pre-call matches the gameplay-rebuild
signature (same camera, same input scale, within 250 ms) and gets divided too. At 130 %:
`1.80000 -> 1.38462` standing, `1.74000 -> 1.33846` aiming; user-confirmed on the
fov100/fov130 comparison sheet. The cinematic routes are untouched by it, and the cutscene
compensation is untouched by the FOV setting.

## AR-011: masked picture stretched over the whole window (2026-09-04)

User report: with Menu or Cutscene Masking on, a 2048x1536 (4:3) window showed the picture
stretched across the full 3840x2160 screen; off, it sat unstretched at the top-left of the
full-screen window. Cause: the game creates the swap chain at the window size and then, at boot,
grows it to the desktop size through `IDXGISwapChain3::ResizeBuffers1`, not `ResizeBuffers`.
Only the latter was hooked, so the band renderer still held the back buffers, the resize failed
silently, and DXGI stretched the small chain to the window. Fix: hook slot 39 (`ResizeBuffers1`)
the same way. Log now shows "swap chain resize (ResizeBuffers1) to 3840x2160, band renderer
dropped" followed by the renderer rebuilt at 3840x2160. Verified at the main menu, 4:3 2048x1536,
masking on: picture in its region, bands top and bottom. The 14:03 session had looked fine only
because a later display-mode change went through the hooked `ResizeBuffers`.
Confirmed by the user in a play session the same day.

## AR-012: D3D11 (2026-09-05)

The game ships with `api = dx11` in `mgs4.ecf` and offers D3D11/D3D12 in its options, so most
players are on D3D11. Under it the bands were disabled: the swap chain's presenter is an
`ID3D11Device`, no D3D12 queue is captured, `BandSetup` bailed ("no command queue"). Everything
engine-side (layout converter, pool append, cameras, resolution, shadows) installs the same on
both. Fix: `BandSetup` asks the chain for an `ID3D11Device` first and, if it gets one, takes the
immediate context as `ID3D11DeviceContext1`; `DrawBands` then fetches buffer 0, makes an RTV,
`ClearView`s the band rects and releases both on the spot, so nothing of ours is outstanding
across a resize (the D3D12 path keeps its command list and per-buffer RTVs). On D3D11 the chain
is created at the desktop size straight away, no ResizeBuffers1 step. Verified at the menu, 4:3
2048x1536, masking on (user set DX11 in the launcher; the harness never touches that setting).
The public side gained a D3D11 `CreateSamplerState` hook for anisotropic filtering; not verified
here because MGSPatriotFix takes that setting on this machine.

## AR-014: the picture never filled the display when the window size was not the desktop size (2026-09-05)

Reported by a tester on a real 4:3 monitor (bars around a squeezed picture) and reproduced by
the user: with the Window Aspect Ratio override the game "always occupies an area of a
3840x2160 window". Cause, measured with a D3D11 context census: the engine's fullscreen is a
borderless window at the desktop size, and the last thing in a frame binds the back buffer,
sets a viewport of (0,0,windowSize) and draws one full-screen triangle - the picture is
windowSize pixels at the origin of the chain, unscaled. The launcher always writes windowSize
= desktop size, so the game never scales on its own; the override is what makes them differ.
Fix: every viewport and scissor the engine sets while the back buffer is bound is remapped
into the largest centred rect of the chain with the window's aspect (`ComputeFit`,
`FitViewport`, `FitScissor`), and the bands are drawn in that rect. D3D11: immediate-context
hooks (OMSetRenderTargets, OMSetRenderTargetsAndUAVs, RSSetViewports, RSSetScissorRects), the
back buffer known by pointer identity. D3D12: CreateRenderTargetView hooked from the chain's
creation to learn the back-buffer RTV handles, then OMSetRenderTargets / RSSetViewports /
RSSetScissorRects on the command-list vtable; bound state per list. The DXGI hooks now install
whenever the window override is on, masking or not. Verified on D3D11: 4:3 1400x1050 -> a
2880x2160 rect at (480,0), 21:9 3840x1620 -> centred at (0,270). D3D12 not yet verified.
The engine still lays out and renders for windowSize; only the last hop scales.

AR-014, second half (same day): the chain is not the desktop size either - it is the game's
configured fullscreen resolution (the launcher's choice, 3840x2160 here), whatever the window
is. On a 1440x1080 desktop the engine made a 1440x1080 borderless window and resized its chain
to 3840x2160, which DXGI then stretches into the window: a 16:9 chain squeezed into 4:3, the
picture with it. `SizeToWindow` (in both ResizeBuffers hooks, only with the window override on)
replaces the requested size with the window's client size, so the chain is the window and the
fit lands 1:1. Verified: 1440x1080 desktop, 4:3 1400x1050 -> "ResizeBuffers asked for
3840x2160; the window is 1440x1080", chain 1440x1080, picture x1.0286, screen filled, menus at
16:9 inside with the bands. The tester's bars were this, not their setup - the user's point.

AR-014, the source (same day, after a lab display-API census): the size the engine resizes its
chain to is the **largest display mode the monitor reports**. At start-up the loop at
mgs4+738315 calls EnumDisplaySettingsW for every mode (869 of them here), keeps the biggest
(3840x2160, the panel's native, listed even with the desktop at 1440x1080), and that is the
windowSize our config hook then sees ("Window Size: 3840x2160 -> ...") and the size the
fullscreen ResizeBuffers asks for. Not the ecf (edited to 1400x1050: no change), not the
launcher save (every field set to 1440x1080: no change), not MGSPatriotFix (removed: no
change), not the desktop (1440x1080 at the time). The window itself is sized from the current
mode (EnumDisplaySettingsW(-1) -> 1440x1080 from mgs4+738C2C / 738138 / 738A1B, and
GetSystemMetrics 0/1 from the DXGI runtime). So stock play only lines up when the desktop runs
at the panel's largest mode. The fix stands as is: chain sized to the window, picture fitted.

AR-014, third part (2026-09-05, capture card as a 1600x1200 primary display, DX12): the largest
mode is not just the chain size - under DX12 the fullscreen step **switches the display to it**
(the 1600x1200 output went to 1920x1080 while the game ran and back when it closed; the window
was 1920x1080 and the fit landed inside a 16:9 mode on a 4:3 screen). Fix, public tree
(`render_pipeline.cpp`, `InstallDisplayModeClamp`, only with the window override on): the
mode walk (`EnumDisplaySettingsW` with non-negative indices) is answered with the current mode
for index 0 and nothing after it, so the game's "largest" is the desktop's mode, its window
is the desktop, and the switch is a no-op. Verified: display stayed 1600x1200, window
1600x1200, chain 1600x1200, picture 1400x1050 scaled x1.1429 to fill it. This is most likely
the tester's case: a 4:3 panel whose driver lists a larger 16:9 mode gets switched to it, and
the monitor letterboxes the result - bars around a squeezed picture.

## AR-015: the D3D12 renderer squeezes the whole picture into a 16:9 band (2026-09-05)

With the three AR-014 layers in place (1600x1200 display, window = chain = display, masking
off) the user's read of two DX12 gameplay captures, layout fix on and off, was "the game is
squished in both". The captures: black bands top and bottom with uncleared garbage in the
corners, scene and HUD squeezed vertically between them. The D3D12 command-list census from
the same boot (frames 150-151) shows the pass: the scene is rendered at 3200x2400 (4:3, 2x),
and the last offscreen pass of the frame binds a window-sized target with a depth buffer and
sets viewport `(0,150 1600x900)` with scissor `(0,150)-(1600,1050)` - the centred 16:9 band
of the window - before the full-size back-buffer blit `(0,0 1600x1200)`. So the 4:3 picture,
scene and interface together, is drawn into a 16:9 band of a 4:3 target and shown as is.
The D3D11 renderer paints the same pass over the full window (AR-014 census) and its 4:3
picture is right.

This also explained the four earlier icon crops that had pointed at layer 1: layer 1 on gives
round icons in the target, which the band squeezes wide by 4/3; layer 1 off (Stretched HUD)
gives icons stretched tall by 4/3, which the band squeezes back to round - two errors
cancelling, not a working configuration. Layer 1 is untouched by this fix; its physical
viewport in the layout object is the render size (`physical 0 0 3200 2400` at 1600x1200),
the logical range `0..1280 x -120..840` as designed.

A caller walk from the band viewport (`band viewport callers: ... mgs4+7A5F6C < mgs4+76628F
< mgs4+7663D7 < mgs4+7664AE < thread start`) ends in the renderer's frame submit, not at a
computation - the rect arrives as data through the generic view path - so the correction is
applied where the values are used. `Fit12_RSSetViewports` / `Fit12_RSSetScissorRects` now run
on every command list: in a window taller than 16:9 (`ww*9 < wh*16`), a viewport that is
exactly the window's centred 16:9 band (`x 0, w ww, h ww*9/16, y (wh-h)/2`, within a pixel)
is widened to `(0,0,ww,wh)`, and the matching scissor to the whole window; the AR-014 fit
then applies on top when the back buffer is bound. `Band Expand` (lab key, `bBandExpand`,
on in Release) switches it off for A/B boots. Log: `fit: the engine's 16:9 band (0,150
1600x900) is widened to the 1600x1200 window.` At 16:9 and wider nothing can match and the
gate is the window aspect; 16:9 and 21:9 overrides on the same display logged no band line.

Verified on DX12, 1600x1200, Expanded HUD: gameplay fills the window, HUD in the window's
corners, compass ring and reticle round in the crops; with menu masking on, the main menu
sits in the 16:9 band with the bands over the rest and round OK/Back icons. The harness's
`in_game.ps1` recognises the 4:3 gameplay layout only with this fix (before it, the squeezed
HUD failed the check and the boot reported "never reached gameplay" with the gameplay
capture in `stuck.png`). User's read of the captures pending at the time of writing.

AR-015, second part (same day): under DX12 with the override not equal to the display (16:9
1600x900 and 21:9 1600x686 in a 1600x1200 chain) the picture sat 1:1 at the top-left of the
chain and only the masking bands landed in the centred rect. Census with the 1400x1050
override (RTV sizes added to the D3D12 census): the scene targets are window-based
(2800x2100 at 2x), but the composition targets are **chain-sized** (1600x1200, with a depth
buffer) while the viewports set on them are window-sized (`(0,0 1400x1050)` and the band
`(0,132 1400x787)`), and the back-buffer blit is `(0,0 1600x1200)` - a 1:1 copy of a
chain-sized target with the picture in its corner. D3D11 sizes those targets by the window,
which is why the AR-014 fit on the back buffer was enough there. Fix: `Fit12_OMSetRenderTargets`
marks a list fit-bound when its target is the back buffer *or* an offscreen target of the
chain's size (`g_RtvSizes12`, filled by the CreateRenderTargetView hook in every build;
`FitRect` carries the chain size), so the window-space viewports on those targets are
fitted and the 1:1 blit carries the fitted picture across; the blit's own chain-sized
viewport is skipped by `FitViewport` as before. `DrawBands` now clears the chain outside the
fitted rect every present the fit is active (the engine's uncleared margins were being copied
over), with the masking bands on top; the rect array grew to six. Verified on DX12: 4:3
1400x1050 -> "picture scaled x1.1429 into a 1600x1200 rect at (0,0)", menu fills the screen
with the 16:9 masking; 21:9 1600x686 -> "1600x686 rect at (0,257)", menu centred with black
above and below and the pillar bands inside. User's read pending at the time of writing. The
tester's DX12 4:3-on-16:9-desktop case goes through this path.

AR-015, third part (2026-09-06): the band goes the other way too. The user set a 2560x1080
desktop and the same in the launcher and saw the picture compressed sideways; the census
showed the centred 16:9 pillar band `(320,0 1920x1080)`, scissor `(320,0)-(2240,1080)`, on
the 2560x1080 composition target. `CurrentWindowBand` now yields the letterbox band for a
window taller than 16:9 and the pillar band (full height, `wh*16/9` wide, centred) for one
wider, and the viewport and scissor matches use all four edges. Verified at 2560x1080 native
on DX12: gameplay fills the width, HUD in the 21:9 corners, compass ring and reticle round.
Note for the harness: a gameplay capture taken within 15 s of a stage-automation load still
shows the boot-phase masking bands (no loaded-save plate on that path, the 15 s camera-alone
fallback), which looks like the band clipping the sides - boot with masking off in the lab
file for a clean read. The earlier 21:9 verifications on the 4K desktop (3840x1620 in a
3840x2160 chain) never met this band because the composition targets were chain-sized there
and the picture was read at 1:1.

AR-015, fourth part (2026-09-06, DX11): D3D11 has the same band. With the user on DX11, the
native 4:3 1600x1200 gameplay capture showed the picture squeezed into the 16:9 band with the
uncleared corners, exactly as DX12 had. The earlier "D3D11 is right at 4:3" was read from
masked menus, where the 16:9 band and the masking coincide and the squeeze is invisible - a
lesson in what a capture can and cannot show. The band helpers moved above the D3D11 section
and `Fit11_RSSetViewports` / `Fit11_RSSetScissorRects` now widen the band on any target and
fit on the back buffer, the same shape as the D3D12 hooks. Verified on DX11 on the 1600x1200
display: native 4:3 gameplay fills the window ("band (0,150 1600x900) is widened"), HUD in the
corners, ring and reticle round; 21:9 1600x686 fitted at (0,257), full width, no band line on
this backend at that size; 4:3 1400x1050 scaled x1.1429 to fill with the masking bands ("band
(0,132 1400x787) is widened"). User's read pending at the time of writing.

AR-015, fifth part (2026-09-07): whose band. The user put a 4:3 display on and reported
"everything is squished" in fullscreen. Their settings file was still on the 16:10 override
from the day's earlier test (2880x1800, capped by WR-002's surface cap to 1600x1000 on the
1600x1200 display), so this was a 16:10 picture on a 4:3 chain, a combination never measured:
every band case above had window = chain (native 4:3, native 21:9), and every fitted case had
a 16:9 chain (the 4K desktop), where the engine draws no band at all. The D3D12 census at
the title (lab, 16:10 lab settings) showed the composition pass into the chain-sized target
with viewport `(0,125 1600x750)`: the 1600x1200 chain's 16:9 band `(0,150 1600x900)` scaled
by 1000/1200 into the 1600x1000 window rect. The matcher (`CurrentWindowBand`) built the band
from the window's own aspect, `(0,50 1600x900)`, matched nothing, and the picture stayed in
the engine's band, then fitted at (0,100). So the engine works the band out from its own
resolution (the chain, since WR-002 keeps them equal) and scales it into the window; the two
formulas coincide only when window = chain. `CurrentWindowBand` now takes the chain's aspect
(`g_Fit.cw/ch`, the window when no chain is known yet), builds the chain's centred 16:9 band
and scales it by (ww/cw, wh/ch); the widening target is still the window, and the fit then
places it in the chain as before. A 16:9 chain yields no band, so the 4K-desktop cases are
untouched. Verified on the 1600x1200 display, DX12, lab: 16:10 1600x1000 "band (0,125
1600x750) - the 1600x1200 chain's band, scaled into the window - is widened", title filling
the (0,100) rect; 21:9 1600x674 "band (0,85 1600x505) ... widened", fitted at (0,263); plain
4:3 1600x1200 unchanged ("band (0,150 1600x900) is widened"). The log line names the chain
when it differs from the window. Not a regression of 0.0.4/0.0.5: 0.0.3 had the same
window-shaped matcher and no cap, so a 2880x1800 window on this display would have failed the
same way. User's read of the before/after captures pending at the time of writing.

## AR-016: codec calls - the portrait camera follows the window's aspect (2026-09-06)

Reported as issue #1 on the public repository (RylicTWG, 3440x1440, DX11, 0.0.2: "codec calls at
the wrong aspect ratio", portrait narrowed and cropped) and confirmed by the user as a
long-standing one ("codec calls have always been compressed into a 16:9 frame either at 4:3 or
ultrawide"), with a second ask: codec calls should be a 16:9 picture with no masking bands.

Measured with the automated call (`scratchpad/codec_run.ps1`: stage boot into s01a10l_1, Tab,
Enter for CODEC, left click for SEND - synthetic Enter is ignored on the codec screen, the UI
confirm is the left mouse button - then `camlog`, `projlog 300`, captures). A call renders the
caller with a second camera, projection scale 4, built on the same routes as the gameplay
camera (+EB0EB, +EB191, +B8A43, +B8B4F, +BA3A3; `aspect=1.0000` on every build). Its projection
at the setter:

| window | m00 | m11 | \|m11/m00\| | aspect x 1.0171 |
| --- | --- | --- | --- | --- |
| 16:9 3840x2160 | 4.00000 | -7.23256 | 1.8081 | 1.8081 |
| 21:9 3840x1620 | 4.00000 | -9.64341 | 2.4109 | 2.4109 |
| 4:3 2880x2160 | 4.00000 | -5.42248 | 1.3556 | 1.3561 |

m00 is fixed and m11 follows the window's aspect, so the pane - laid out at 16:9 like the rest
of the codec - receives the window's shape squeezed into 16:9. The gameplay camera behind the
pause menu has |m11/m00| equal to the window's aspect exactly, without the 1.0171, which is the
discriminator.

Fix (`PatchCodecPortrait`, called from the projection setter hook): while a screen is live
(`ScreenLive()`), a matrix whose |m11/m00| is the window's aspect x 1.0171 within 0.5 % has m11
replaced by copysign(|m00| x 16/9 x 1.0171), i.e. its 16:9 value; the render target keeps the
window's shape, so the frame is stretched sideways there and the pane's squeeze undoes it. Each
sighting stamps `g_CodecCallTick` (`CodecCallLive()`, 150 ms); a first version took the screen
term out of the band gate with it, so the call ran unmasked, and the user's read of that was
that the bands vanishing at the moment the call connects is a visible jump - the gate is back
to `ScreenLive()` alone and the bands stay up through the call like the rest of the codec.
`CameraHooksWanted()` now includes `bFixUiAspect` so the setter hook is present without any
masking. Lab key `Codec Portrait Fix` (`bCodecPortraitFix`, on in Release). Log line once:
`codec portrait camera m00=... held at 16:9: m11=...`. The transition also carries an m00 = 3
camera with the same signature (the codec zoom-in); it gets the same treatment, which is what
it needs.

Verified on DX11: 21:9 3840x1620 and 4:3 2880x2160, Otacon call from the Middle East stage -
portrait framed as at 16:9 inside the masked codec; the 16:9 window is untouched by
construction. User's read of the 21:9 and 4:3 calls: "Looks like that fixed it." Not yet run at the
reporter's exact shape (a native 21:9 desktop, window = chain, where the AR-015 band widening
also fires); the two fixes act on different passes, and the native 4:3 case with the band
widening active (1600x1200 capture output as primary, DX11, 2026-09-06) shows the call
framed as at 16:9 inside the masked codec, so the two do not interact. The reporter's shape
followed the same day: native 1920x823 (the one 21:9 mode the capture output would take),
window = chain = display, band `(228,0 1463x823)` widened on the same frames - the call is
framed as at 16:9, the pillar bands are pure black in the pixel data (the codec's vignette
makes them look lighter in a capture). The opening video at that size: `movie rect (0,0
3840x1644) -> (459,0 2922x1644)`, pillarboxed, proportions natural. DX12 at the same size,
after the user switched the API: codec call, video and difficulty screen all matched the
DX11 captures (the three hooks are engine-side; the band widening fired on the same frames).

## AR-017: pre-rendered videos stretched to the window (2026-09-06)

The opening's TV commercials (NEW GAME, before the prologue) and the other pre-rendered videos
are drawn as one native rect covering the whole render surface - `(0,0 7680x3240)` at
3840x1620 with 2x scale - so a 16:9 video takes the window's shape: stretched sideways at
21:9, squashed at 4:3, and in a fitted window drawn over the whole chain, ignoring the window
rect and the masking (which never fires, there is no cinematic camera). The user's report:
"FMV cutscenes are getting stretched".

Found with the rect emitter probe (`rectlog`): during the video two return sites emit a full
rect every frame, +E367BB and +921AD. The lab command `rectfix <rva>` rewrote one site's
full rect to the centred 16:9 rect of the surface: +E367BB moved the video into that rect
(openfix_E367BB, 21:9), +921AD changed nothing visible (a full-surface plate under it). The
emitter itself (`+BE090`, `emit(dest, x, y, w, h)` writing a 16-byte packet) is the
mgs4Ultra120-published function, attributed in the source; the site and its use are ours.

Fix (`HookedRectEmit` / `InstallRectEmitter`, installed with the HUD fix in every build, not
only the lab): a full-surface rect from `kRvaMovieRectSite` (+E367BB) becomes the centred 16:9
rect of the surface (pillarboxed when wider, letterboxed when taller, untouched at 16:9). The
site is verified at install as an E8 call to the emitter five bytes before the return address;
otherwise a warning and the videos are left alone. Log line once: `movie rect (0,0 7680x3240)
-> (960,0 5760x3240), the video keeps 16:9.` Verified on DX11: 21:9 3840x1620 -> video at
(960,0 5760x3240) in render units, the whole window's height, black either side; 4:3
2880x2160 -> `(0,540 5760x3240)`, letterboxed. Faces natural in both. One 21:9 run came back
dark on every frame with a "HIDEO 1" channel tag and the same log lines - a channel-switch
accident of the key schedule, not the fix; the repeat run was fine. Also verified at native 4:3
(1600x1200 capture output as primary, window = chain = display, the AR-015 band widening
firing on the same frames): `movie rect (0,0 3200x2400) -> (0,300 3200x1800)`, video
letterboxed with natural proportions. User's read pending at the time of writing.

The harness route: `scratchpad/opening_run.ps1` - boot to the main menu, Up twice, Enter
(NEW GAME), Enter (difficulty), Enter, then captures every N seconds; the video starts ~10 s
after the difficulty select and runs for a couple of minutes. The prologue drama stages
(`s00a00l_D`, `s00a10l_D..D9`) are engine cutscenes, not videos.

## AR-018: an overlay's swap chain took the queue slot, so D3D12 lost its fit and bands (2026-09-10)

A player's log and screenshots: on DirectX 12 the picture was stretched vertically at every
shape (4:3, 16:9, 21:9) and the log read `swap chain hooked (...), queue MISSING` then `no
command queue captured at swap chain creation` and `bands disabled for this session`. Fine on
DirectX 11. The user reproduced it with RTSS and its overlay on; the player says they run no
RTSS, so another overlay (Steam's recording is their guess) is doing the same thing.

Mechanism, from the reproduction's log: the `queue MISSING` line lands before the game has
created either of its devices. RTSS creates a throwaway swap chain of its own (to find the
Present vtable) before the game does, and `HookSwapChain` latched on the first chain it saw:
the queue was taken from that chain's presenter only, which is not a D3D12 queue, and the
game's real chain was never asked. No queue means no band renderer, and on D3D12 the fit
hooks (`InstallFit12Lists`) hang off the band renderer's command list, so the fit never ran
either: the engine's picture was left as it draws it, which at a fitted window is the
stretch. The first failure also set a permanent `failed` flag.

Fix, three parts, none of them changing the no-overlay path:
- `HookSwapChain` takes the queue from every chain created: the presenter when it is a
  queue; otherwise, for a chain on a D3D12 device, the game's own direct queue, which the
  public module now records at `ID3D12Device::CreateCommandQueue`
  (`RenderPipeline::pDirectQueue12`). The vtable hooks stay once per class; a further chain
  is logged with whether its vtable is the same, so a chain of another class would show.
  `InstallFit12Device` updates the fit's chain every time and installs its hook once.
- `BandSetup` falls back to the recorded direct queue too.
- A band failure sticks to the chain it happened on (`failedChain`); another chain presenting
  gets its own attempt.
- `WindowClientSize` no longer lets a hidden window (the throwaway chain's) displace a visible
  one already recorded as the game window.

Verified with the RTSS overlay on for mgs4 (the per-game profile had it off, which is why
the first two attempts here did not reproduce): Release build, 21:9 3440x1440 borderless on
the 3840x2160 desktop, DirectX 12. Log: first chain `queue not needed (not a D3D12 chain)`,
then `D3D12 direct command queue recorded`, `another swap chain created (same vtable), queue
captured`, band renderer ready, `fit: 3440x1440 picture scaled x1.1163 into a 3840x1607 rect
at (0,276)` - the same lines as a run without the overlay. Verified by the user on
2026-09-11 from gameplay captures at the reporter's own configuration, a native 3440x1440
desktop (the capture card's EDID overridden with CRU), RTSS overlay on, 21:9 override at
3440x1440, DirectX 12 borderless: "the first run looked fine". The reporter's odd first
chain (3440x1421 before the resize to 3440x1440) reproduces on this machine too and is only
the start-up transient.

## AR-019: "Use Game Setting" still ran the fit and the band widening, and the HUD spread followed the window (2026-09-11)

Found while verifying AR-018 at native 3440x1440 with Window Aspect Ratio on "Use Game
Setting". The engine then renders at the game's own resolution (2560x1440, a 16:9 buffer,
`render buffer 2560x1440 -> 5120x2880 ... Output stays at 3440x1440`) and pillarboxes it
into the 21:9 window, which is stock behaviour. Two things of ours were wrong with that:
the AR-015 widening matched the engine's pillarbox band and stretched the 16:9 picture to
the window (user: "everything is stretched vertically", the same read as the player's
screenshots, though theirs was AR-018), and the HUD spread, keyed on the window size, laid
the interface out for 21:9 inside a 16:9 picture, so the engine's band squeezed it ("we are
squeezing a 21:9 hud into a 16:9 pillarbox, while maintaining a 16:9 render").

Rule, from the user: "Use Game Setting" for the aspect and window mode disables every
resolution and scaling hook, supersampling aside - those settings live under Ultrawide and
4:3 for a reason - and "any time the game is rendering in a 16:9 resolution, there is no
reason to have the expanded hud". The render and the HUD are never to be stretched or
compressed.

Fix: `ComputeFit` and `CurrentWindowBand` return inactive unless `bOverrideWindowSize`
(the fit and the widening join `SizeToWindow` and the mode clamp, which were already gated);
and the HUD spread (`HorizontalSpreadFactor`, `VerticalSpreadFactor`, the layout range in
the layer-1 hook, the F11 report) reads the render buffer's shape (`PictureSize`,
`render.bufferSizeX/Y`) instead of the window's. With an override the two shapes are the
same and nothing changes: the override run's log shows `UI Aspect: 6880x2880 output`, the
band widened, the compass anchored by 0.2558 as before. Without one a 16:9 render gets a
16:9 interface and the Expanded HUD setting is inert.

A first attempt at the widening (leave the band alone when the render buffer is 16:9,
keep everything else) was withdrawn the same day: it kept the pillarbox but still spread
the HUD for the window, and it was not the rule.

Verified by the user, 2026-09-11, from two Lab captures at the native 3440x1440 desktop,
DirectX 12 borderless, RTSS overlay on: "Use Game Setting" (16:9 render pillarboxed, 16:9
HUD, no fit, no widening) and the 21:9 override (unchanged): "Yes, these are correct".
