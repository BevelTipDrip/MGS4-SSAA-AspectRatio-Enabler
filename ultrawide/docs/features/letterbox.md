# Letterbox: cutscenes and menus kept at 16:9 (AR-010)

**Status:** working in the lab at 21:9 and 4:3, 2026-09-04. Switch: `Aspect Letterbox=1`
(`[Graphics]`, lab settings only; `bAspectLetterbox` in `aspect_ratio.hpp`). Code: the
"Letterbox" section of `src/fixes/aspect_ratio.cpp` and the screen table just above
`ShiftAppend`.

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

`BandsWanted()` (live `box 0|1|2`: gated, forced on, forced off). Gated means either signal
within the last 150 ms:

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

Log lines to look for: `letterbox: screen element XXXXXXXX (...) seen; bands follow it.`
(once per identity per boot) and `boxstat`'s `cutscene-live yes|no screen-live yes|no (last
XXXXXXXX)`.

## 4. Lab

- `cutscene_run.ps1 -LiveAt "sec:command"` - boots a stage's opening cutscene (rewrites
  `Stage Automation Stage` in the lab settings), fires live commands at given seconds,
  captures on a schedule, closes the game.
- `state_census.ps1 -Phase gameplay|codec|title` - boots (the barrel save, or to the title),
  walks the screen states with `send_input.ps1` (Tab = pause menu, Enter = confirm, RClick =
  UI cancel, Esc = BASIC CONTROLS), and at each state writes `boxstat` + `sites` and
  captures. Copies the game log next to the captures at the end (the log is fresh per boot).
- `sites_census.ps1 [-LogPath] [-Against <dump>] [-Full]` - tabulates each `sites` dump by
  identity (owner, channel, node strcode, la2 object) with counts and x/y ranges; `-Against`
  prints only what a dump has that the reference does not. Dump 0 is usually empty (armed
  before any append), compare against 1+.
- `sheet.ps1 -Dir <captures> -Names a,b,c -Crop 'W,H'` - contact sheet of a run's captures,
  cropped to the window (the captures are desktop-sized).
- Live: `box`, `cutzoom 0|1`, `cutdiv <f>`, `camlog N`, `projlog N`, `boxstat`.

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
- **Shipping** - all of this is lab-only behind `Aspect Letterbox`.
