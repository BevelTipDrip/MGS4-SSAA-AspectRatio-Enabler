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
  Afevis's HUD fixups alter the vertices or the ortho awaits a census with 
