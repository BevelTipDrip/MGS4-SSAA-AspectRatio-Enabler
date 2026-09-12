# Peace Walker engine notes for the aspect work

Research moved out of the public `docs/pw/engine-research.md` (2026-09-13): how the UI is
drawn, what identifies an element, and how Afevis's patch centres the HUD. Evidence levels
as in the public docs. The method and the register are in `ui-elements.md`.

## The UI at the title (Measured, 2026-09-12, 3440x1440 window, Afevis's mod present)

- **The UI is quads in a centred 480x272 canvas.** Every UI draw is `Draw` (not indexed),
  triangle list, from the same native site `+57CA9` after a vertex upload at `+57596` (a `Map`
  with discard on the deferred context, called from `+212E5`). Vertex layout: 24 bytes textured
  (`u, v` float; `rgba8`; `x, y, z` float) or 16 bytes untextured (`rgba8`; `x, y, z`). x runs
  -240..240 and y -136..136 for canvas-filling quads. The vertex shader's first constant
  buffer holds the ortho: `2/480, 0, 0, 0 | 0, -2/272, 0, 0 | 0, 0, -0.002, 0 | -1, 1, -1, 1`
  (row-major with the translation in the last row; the centring must be applied by the shader
  or the vertex builder). So 480 canvas units span the whole window width whatever its shape:
  at 21:9 the canvas is stretched 1.35x unless something changes the ortho or the vertices.
  Whether the title picture looks stretched on screen is the user's read of the capture, not
  inferred here.
- **Native callers do not identify elements.** Every quad's stack is
  `+57CA9 < +5A166 < +5B033 < +22686 < +22686 < +1C96F < +544D2 < +78195 < +7BBB9 < +79AB4`:
  `+22686` recursing is a display-list interpreter replaying what the game logic queued
  earlier. Owner attribution by unwinding, the MGS4 method, has no purchase here. What does
  identify an element, the element tables and how one is moved are in the private
  ultrawide module (`external\ultrawide\pw\docs\ui-elements.md`).


## Gameplay and menus (Measured, 2026-09-12, route replay, 6880x2880 window)

- **Gameplay renders straight into the output-sized target too.** A mission frame is ~8,800
  draws, all on the 6880x2880 back buffer (plus a 192x192 blur chain); 285 of them are UI
  draws with the ortho constants. There is no separate internal 3D target to resize: with
  Afevis's patches the world is drawn at window size, which is why his supersampling has to be
  a bigger window.
- **Each UI draw uploads its own 2640-byte vertex constant buffer** (the ortho in rows 1-4
  and a world matrix in rows 5-8), followed by a 352-byte one and, for quads, the vertex
  upload, all through `Map` with discard on the deferred context. Text is `DrawIndexed` from
  a shared vertex buffer. The census's per-draw call trace shows the texture bound before
  the uploads.
- **The menus and the mission HUD were tabled** with the census on 2026-09-12 (Mission
  Selector, in-mission HUD) and moving them live was verified in the mission; the tables,
  the handle and the biases are in the private module.

## How Afevis's patch centres the HUD (Reported: full read of his dllmain.cpp, 2026-09-12)

## The resolution plumbing (Reported: Afevis's MGSPW-Resolution-Unlocked source, MIT, 2026-09-12)

The game renders on the PSP's 480x272 canvas. Afevis's patch keeps the canvas height, widens
it to the output's aspect (an "internal" size of e.g. 650x272 at 21:9), sets a render scale of
output height over internal height, and patches, all by byte pattern at init: the preset
resolution tables (the game's own upscale slots and a windowed table, overwritten with the
custom size), nine render-target creation and getter sites (register or stack rewrites), the
render-scale reads, about a dozen HUD element fixups (scaler, offsets, grenade arc, hit
markers, movies, loading screens, colour filter, glare, item markers, damage overlay), and the
FOV and culling FOV divided by the UI scale when wider than 16:9. Its log on this machine
(3440x1440 desktop): all 28 patterns found, output 3440x1440, internal 650x272, render scale
5.29. Afevis says it has bugs. The HUD fixups are per-element register patches; the MGS4
identity method would replace them.

### How it does each thing (Reported: full read of dllmain.cpp, 656 lines, 2026-09-12)

- **Window and swap chain: not touched.** The only window-related patches overwrite two
  preset tables the game already has: the fullscreen table (four slots the launcher's
  `-resolution` picks from: 720p, 1080p, 1440p, 4K, at `+1C8EB`) and the windowed table (first
  four of five slots, `+1AAAF`), all set to the output size. Whether the game runs windowed or
  full screen, and which slot it uses, stays the launcher's business (`-resolution 0|1`).
  There is no hook on window creation, style, position or DXGI.
- **Render resolution: the internal render target becomes the output size.** Nine sites:
  the "internal RT" size (`+5C24D`, a packed width|height register), five getters (`GetRT_1..5`)
  that return width and height in registers or a stack slot, `CreateRT` (`+89F8D`, replaces a
  target described as internal-canvas-sized with the output size) and `CreateRT2` (`+7890C`,
  replaces a literal 480x272 with the internal canvas). The scale between canvas and pixels is
  one float, output height / canvas height (5.29 at 1440p), forced at `Game Scaler` (`+596E5`)
  and `GetRenderScale` (`+5B015`) and at the `HUD: Scaler` site (`+563E3`). "Supersampling" in
  his ini is therefore only a larger custom output size: both preset tables, the internal
  target and the scale follow it together, so the swap chain very likely follows it too (not
  measured: on this machine his ini reads `6,880` and `2,880`, which inipp rejects because of
  the commas, and his log shows `Custom Resolution: true (0x0)` falling back to the desktop
  3440x1440; the census saw a 3440x1440 swap chain and 3440x1440 targets).
- **HUD centring: the canvas is widened, not the picture.** The canvas becomes 650x272 at
  21:9 (480 x aspect / 1.7647), 480x360 at 4:3 (272 / (aspect / 1.7647)). The game's UI still
  lays out in 480x272 and is placed in the middle of the wider canvas by an offset of
  (internal - 480) / 2 and (internal - 272) / 2 written as int16 pairs into the camera/viewport
  structure (`Camera Config`, `+8EA0C`, offsets +580/+582 when a magic at +576 matches) and as
  a PSP-style viewport centre of 2048 + offset for loading screens (`+78603`); movies get the
  same offset in registers (`+440FAB`). `HUD: Offset` (`+8EDE3`) zeroes a computed offset so the
  game's own centring does not double up. The 3D camera gets the new inverse aspect and the
  canvas size in its structure (`Aspect Ratio`, `+8EE22`, float at +592, uint16 pair at
  +584/+586) and the FOV and culling FOV are divided by the UI scale when wider than 16:9, so
  the world fills the wider canvas. **No manual pillarboxing or letterboxing exists anywhere**:
  at 4:3 the canvas grows taller and the UI sits centred with world above and below.
- **The per-element fixups are for things anchored to the canvas size or edges**: colour
  filter (`+2EF2C8`, uint16 canvas size into a structure), damage overlay (`+491F0B`, eight
  fields of canvas size), glare and half glare (`+19469B`, `+194A5A`, an edge term scaled by
  the UI scale, `240 * scale - x` for the mirrored half), grenade arc and hit markers (one
  register scaled by the UI scale), item pickup markers (`+FEE61`, the world-to-screen result
  shifted back by half the widening). These are the elements that would otherwise sit on the
  480-wide edges or stretch over the 650-wide canvas.
- **Scissoring** (`+5A31F`): the scissor width (wider than native) or height (narrower) is
  multiplied by 2 x UI scale; this is the one place the output-shaped rectangle is derived.
- **Canvas units are carried in 16-bit fields** in the camera structure, the colour filter and
  the damage overlay (int16 offsets, uint16 sizes) and the PSP viewport convention centres at
  2048 in a 4096-unit space. The canvas only widens with aspect (650 at 21:9, 966 at 32:9),
  never with resolution: pixels come from the float render scale and the int32 target sizes.
  So supersampling does not push those 16-bit fields the way MGS4's 12.4 vertex range was
  pushed, and the UI vertex data itself is float (measured, 24-byte layout). The 16-bit risk
  is confined to aspect, and only at widths beyond anything a monitor has.
- **Open**: the census read `2/480` in the UI vertex shader's first constant buffer with his
  ASI loaded and the canvas at 650 wide, and the D3D viewport at the full 3440x1440 for every
  UI draw; the centring must arrive through a constant the census did not dump (rows beyond
  the first four, or another buffer slot) or inside the vertex builder. A census that dumps
  the whole buffer, with and without his ASI, settles it.


## Open

- The perspective builder. The UI ortho is measured (480x272 over the whole viewport); whether
  the HUD looks stretched at 21:9 awaits the user's read of the title capture, and whether
  Afevis's HUD fixups alter the vertices or the ortho awaits a census with his ASI removed. 

## Window, display mode and resolution plumbing (Measured, 2026-09-13, live `.text`/`.rdata`/`.data` dump at the title, Afevis's ASI loaded)

Read from the decrypted dump with `pw\tools\sites2.py`, `refs.py` and `winres_scan.py`
(import thunks resolved from the live hint/name table, since the on-disk import directory is
inside the encrypted range). Addresses are module RVAs.

**The display object** is one heap struct, pointer at `+15969D8`, and the whole window story
is its fields:

| Field | Meaning | Source |
| --- | --- | --- |
| `+0x2960` | window mode: 0 windowed, 1 fullscreen window on the monitor rect (frame stripped by `SetWindowLongA`), 2 borderless at the monitor's largest display mode **with an exclusive-fullscreen swap chain** | setting 3 |
| `+0x2964` | previous mode | |
| `+0x2968` | applied mode | setting 246 (`0xf6`) |
| `+0x296c` | `-resolution` (0 Original, 1 FHD) | launcher argument |
| `+0x2970` | `-upscale` slot 0..3 into the output table `+D8F1C8` (720p, 1080p, 1440p, 4K; clamped to at least `+0x296c`) | launcher argument, set at `+1A5E3` |
| `+0x2974` | windowed preset index into `+10399E0` (0 = custom size) | setting 19 (`0x13`) |
| `+0x2978` | monitor index (-1 auto) | setting 17 (`0x11`) |
| `+0x2950/54` | window x, y | settings 4, 5 |
| `+0x2958/5c` | window client width, height | settings 7, 8 |
| `+0x2940/44` | picture width, height inside the window | computed at `+1A2B5` |
| `+0x2948/4c` | picture x, y offset (pillarbox / letterbox) | computed at `+1A2B5` |
| `+0x2988/90` | monitor list (0x60 bytes each: RECT, work RECT, a vector of display modes at +0x48/+0x50) | `EnumDisplayMonitors` at `+1A5D6`, callback `+1A3E0`, modes from `EnumDisplaySettingsA` at `+1A6DF` |
| `+0x29a0` | `DXGI_SWAP_CHAIN_DESC`: width and height **0** (client size), 60/1, format 28, 1 sample, usage 0x20, 2 buffers, hwnd `+107BD50`, `Windowed = (mode != 2)`, effect FLIP_DISCARD (4) or DISCARD (0) in mode 2 | filled at `+18A58..+18B62` |
| `+0x2938` | the swap chain | `CreateSwapChain` at `+19180` (factory slot 10), then `MakeWindowAssociation(flags 3)` at `+191DB` |

**Settings are an int array** at `*(+10C58A0)`, loaded by `+84D60` (0x1128 bytes from a file
found through `+9548D0`), read by `+84D50(id)` and written by `+85130(id, value)`; ids used by
the window code: 3 mode, 4/5 position, 7/8 size, 17 monitor, 19 windowed preset, 246 applied
mode. Window creation (`+773B5..+77561`) registers the class, reads 4, 5, 7, 8, calls
`CreateWindowExA` with style `0xCF0000` (`WS_OVERLAPPEDWINDOW`) and then `SetWindowPos` with
the client-area correction; `SetProcessDpiAwarenessContext(-4)` (per-monitor v2) at `+77319`.
`GetWindowPlacement` at `+76633`/`+76677` only tests for the minimised state. The runtime mode
switch handler `+17770` compares old and new mode against 2, calls `SetFullscreenState`
(`+17811`, `+178A0`), releases and recreates the chain (`+178B3` -> `+19120`) or
`ResizeBuffers` (`+178D3`), then persists mode and index through `+85130`.

**The windowed size** (`+19DAB`): preset index 0 means the custom size from settings 7/8;
otherwise the entry of `+10399E0` (five entries; the game itself writes the custom size into
entry 0 at `+19E58`/`+18B93`, and `+1A6F0` validates the presets against the monitor's mode
list), clamped to the monitor rect. `+1AA40` picks the largest preset that fits the monitor.

**The picture fit** (`+1A2B5..+1A360`), run after every window sizing: takes the output table
entry `+D8F1C8[+0x2970]` as the picture, compares window/picture width and height ratios, and
stores the picture size in `+0x2940/44` and the centring offsets in `+0x2948/4c`, i.e. **the
game letterboxes or pillarboxes the upscale-table aspect inside the window** (this is the
Peace Walker equivalent of MGS4's 16:9 band). `+5C7A5` and `+548A6` read the same entry for
the render targets, `+15A10` for the internal target and the "needs upscale pass" flag
(`+0x3338 = (resolution != upscale)`).

**What Afevis's patch does in these terms.** It overwrites all four output-table entries and
four of the five windowed entries with one custom size, so the fit becomes the identity
(picture = window, offsets 0) and the render targets equal the window; it never touches mode,
monitor, position or the swap chain. Consequences he did not handle:

- **Mode 2 is exclusive fullscreen** (`Windowed = FALSE`, DISCARD): a custom size the display
  cannot present (6880x2880) is only safe in modes 0 and 1; his bigger-window supersampling
  works because the chain is windowed and DWM scales it.
- **The windowed preset chooser** (`+1AA40`, largest preset that fits the monitor) has nothing
  that fits once the entries are 6880x2880, so it falls back to index 0 (custom), which is why
  his ini size still applies; the launcher's windowed presets are gone.
- **The picture fit is bypassed, not fixed**: at any aspect other than the custom size's the
  game would still letterbox. A proper aspect feature hooks `+1A2B5` (or the readers of
  `+0x2940..+0x294c`) the way our MGS4 band work fits the picture, and leaves the tables alone.
- **Window mode, monitor and size overrides have a clean seam**: the settings getter
  `+84D50` for ids 3, 4, 5, 7, 8, 17, 19 (the MGS4 `+65D880` loader wrap equivalent), or the
  fields of the display object before `+18A46` runs. No byte pattern on a table is needed for
  a Window Resolution or Window Mode feature.
- **Refresh rate is fixed at 60/1** in the desc; the mode-2 path picks the largest mode by size
  only (`+1A220` loop keeps max width/height, no refresh comparison).
- The 16-bit `movsx` at the half-glare site is unrelated to window size (that is the HUD).

Strings that looked relevant and are not: `"invalid window size"` is zlib's inflate error;
`area_width`/`area_height` are PSP game-data keys; `[monitor_number]` is a Mother Base UI
placeholder. The launcher vocabulary in `.rdata` is exactly `launcherpath, launcherroot,
region, selfregion, ctrltype, invited, resolution, movie, upscale` (`+DA8AB0..+DA8B20`).

## The native render pipeline (Measured, 2026-09-13, Lab census with Afevis's ASI disabled)

Two launcher configurations censused at the title (`Internal Resolution (PW)` / `Internal
Upscaling (PW)` through PatriotFix's launcher skip; both restored afterwards):

**Original / Original** (`-resolution 0 -upscale 0`): window client 1720x720 (the game sized
it), swap chain 1720x720. Every draw, world and UI, lands on a **1440x816** target (the PSP
canvas at exactly 3x) which is **multisampled**: the frame ends with `ResolveSubresource`
1440x816 -> 1440x816 (`+5C4ED`), then one `Draw n=4` (vs `4f1bc59e`, ps `4c3c9a5f`) samples
the resolved texture into the back buffer with the fit viewport `(220,0 1280x720)`: a plain
textured blit, downscaling 1440x816 to 1280x720, pillarboxed inside the 21:9 window by the
game's own fit (`+1A2B5`).

**FHD / WQHD** (`-resolution 1 -upscale 2`): window 3440x1440. Everything draws on
**1920x1088** (canvas at 4x, height rounded up to 1088). Then, per frame: resolve
1920x1088 MSAA (`+5C4ED`); **compute `Dispatch 160x90`** (cs `cc0b8cd0`, SRV = the resolved
1920x1088, UAV = a **2560x1440** texture; called from `+1B177 < +1ADFE < +5C603`): the
upscaler; `CopyResource` 2560 -> 2560 (`+1AED4`); a second **`Dispatch 160x90`** (cs
`78e5b70a`, SRV = the copy, UAV = the same 2560 texture; `+1AFD9`): a sharpening pass (the
two-pass shape of FidelityFX Super Resolution 1: EASU then RCAS; 160x90 = 2560x1440 / 16);
then the final `Draw n=4` samples the 2560x1440 texture into the back buffer at the fit
viewport `(440,0 2560x1440)`. The compute path runs only when `-resolution != -upscale`
(`+0x3338`, set at `+15A1E`); with equal indices the resolved internal target is blitted
straight into the fit rectangle.

**The internal size is an immediate, not a table**: `-resolution 0` -> 1440x816
(`0x5A0`, `0x330`), `-resolution 1` -> 1920x1088 (`0x780`, `0x440`), at every reader of
`+0x296c` (`+1620D/+16212` and `+1621A/+16220`, `+5C243` packed as `0x330000005A0`,
`+1AC49..+1AC5D`, and the rest of the nine sites Afevis mid-hooks). The output size is the
table `+D8F1C8[+0x2970]` (720p, 1080p, 1440p, 4K; `.rdata`, writable with `VirtualProtect`).
The canvas-to-pixel scale is internal height / 272 (3.0, 4.0); the HUD draws into the same
internal target as the world.

**Supersampling without an oversized window, from these facts:**

1. Set the internal size at its immediate sites to `k x` the window client size (k = 2, 3:
   integer, so the final bilinear blit is an exact box filter; MSAA on the internal target
   stays as the game has it).
2. Set the output table entry for the running `-upscale` slot to the window client size, so
   the game's fit is the identity (no bars, no offset) and the final blit is a k:1 downscale
   into the whole back buffer.
3. Keep `-resolution == -upscale` so the compute upscaler stays off (or, later, drive it
   deliberately: EASU is an upscaler, its kernel is not what a downscale wants).
4. Leave the windowed preset table alone: window size and mode stay the user's (or our Window
   Resolution feature's) business, exactly as in MGS4.
5. The 3D projection and the canvas widening are separate (aspect) concerns; at 16:9 nothing
   else moves. At 21:9 the canvas widening (Afevis's `ComputeScaling`, or ours) still applies,
   with `k` multiplying the render scale, never the canvas.

What is not yet measured: the MSAA sample count of the internal target (a `CreateTexture2D`
census would say), whether the final blit shader is bilinear (its 8-byte hash `4c3c9a5f` is
one of the eleven `.cso` files; D3DReflect or the bytecode tells), and the cost at k = 2 on
a 3440x1440 window (6880x2880 internal, the same pixel count Afevis's oversized window
already renders, so it is known to run).

## GPU cost per pass (Measured, 2026-09-13, timestamp census, first mission, 3840x2160 @ 60 Hz, MSAA off, Afevis off, game's own blit)

One frame, GPU milliseconds between consecutive stamps (`Draw Census` with `Time Passes`):

| | scale 12 (5760x3264, 18.8 Mpx) | scale 8 (3840x2176, 8.4 Mpx) |
| --- | --- | --- |
| scene command list (world + HUD draws) | 3.13 | 2.27 |
| end-of-frame resolve (a copy with MSAA off) | 1.85 | 1.68 |
| final blit into the window (`4f1bc59e`/`4c3c9a5f`) | 1.10 | 0.58 |
| HDR pass (`5aa8ee43`/`2752864a`) | 0.76 | 0.31 |
| composite (`80fc4e25`/`fcb43266`) | 0.72 | 0.51 |
| two small passes (`baa9f4b4`, `6c27b4cf`) | 0.32 | 0.27 |
| **total attributable** | **~7.9** | **~5.6** |

The 26 ms "frame start" interval is the vsync wait, not work. Reading: the post passes scale
with pixels (2.9 vs 1.7 ms), the scene list less than linearly (fill-bound), and the
resolve/copy costs ~1.7 ms at either size, so it is a pipeline drain at the list boundary
rather than bandwidth. At scale 8 the frame's GPU work is ~5.6 ms of 16.7 (the user's 35-40%
reading); at scale 12 ~7.9 ms (65%). The compositor-scaled flip-model back buffer and
Afevis's oversized window cost about the same as the game's own blit (user readings, one
frame each). Where to save: the HDR and composite passes could run at output size on a
downscaled copy; the resolve/copy cost is structural.

## GPU cost at 8K, averaged (Measured, 2026-09-13, 60 timed frames each, first mission while aiming, 3840x2160 @ 60 Hz window, render scale 16 = 7680x4352, Afevis off)

| Configuration | mean GPU ms/frame | max |
| --- | --- | --- |
| MSAA off, CPU-writable frame textures as the game makes them | 10.1 | 33 |
| MSAA off, those textures made GPU-local (`GPU Local Textures`) | 8.8 | 16 |
| game's 4x MSAA on, GPU-local | 15.0 | 20 |

Per pass with MSAA on: the two `6c27b4cf` full-screen draws 4.2 ms, `baa9f4b4` 3.1, the
resolve 2.5, `fcb43266` 1.6, the three scene lists 1.5, the HDR pass `2752864a` 1.1, the final
blit 0.9. With MSAA off the same passes cost 1.1-1.5 ms each. So at 16x the canvas, 4x MSAA
costs ~6 ms of 15 and buys nothing SSAA does not already do; the post chain (five full-screen
passes plus the resolve) is ~7 ms; the scene itself (thousands of world draws plus the HUD)
under 2 ms.

**The CPU-writable textures**: the game creates ~27 full-size default textures with
`CPUAccessFlags = WRITE` (bind SRV only, formats 28/46/47, the PSP VRAM mirror); a GPU copy
into one goes to system memory over PCIe (134 MB at 8K). In these runs the game never mapped
a large one (11 maps in a run, all a 1280x720 video texture), so stripping the CPU access
is safe here and saves ~1.3 ms. The end-of-frame resolve/copy at 2-2.6 ms mean is mostly the
pipeline drain of the scene list before it, not bandwidth (the same-size on-GPU copy elsewhere
in the frame costs 0.15 ms).

**Depth of field** (`493494fd` at 5.1 ms and the full-resolution `7023c633` at 3.6 ms, seen in
single heavy frames) did not appear in the averaged aiming windows; it is scene dependent and
the heaviest thing in the chain when it runs.

**Where a real saving is**: run the post chain at output size. The six "internal size"
immediates size the resolved/post targets separately from the scene scale, so with a
downscale draw in place of the resolve (scene 7680x4352 -> post 3840x2160) every post pass
would run at 4K (÷4.3) and the final blit 1:1; the depth read by the depth-of-field passes
must then be the scene depth sampled by UV, which needs checking in the shader bytecode.
