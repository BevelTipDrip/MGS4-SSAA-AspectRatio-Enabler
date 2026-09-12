# Peace Walker engine research

Facts about the game's engine, each with its evidence level (see `docs/features/README.md`).
Nothing here is a fix; it is what the fixes will stand on.

## The build (Measured, 2026-09-12, from the files on disk)

- `mgspw\METAL GEAR SOLID PEACE WALKER.exe`: x64, 18.4 MB, image base 0x140000000, size of
  image 0x19A2000. Sections: `.text` RVA 0x1000 raw size 0x98B800 (entropy 8.0 on disk: Steam
  DRM, the `.bind` section holds the entry stub, exactly as MGS4); `.rdata` RVA 0x98D000 raw
  0x50E400 (entropy 7.5: only partly readable on disk, so a string's absence proves nothing);
  `.data` RVA 0xE9C000.
- Imports visible on disk: `d3d11.dll` (`D3D11CreateDevice`), `dxgi.dll` (`CreateDXGIFactory1`),
  `D3DCOMPILER_47.dll` (`D3DReflect`: constant-buffer layouts are discovered at run time),
  `winmm.dll` (the Ultimate ASI Loader works as `mgspw\winmm.dll`), `steam_api64.dll`. No
  `d3d12.dll` string visible; the store page requires DirectX 12 and the launcher ships the
  D3D12 Agility runtime (`launcher\D3D12\D3D12Core.dll`) as MGS4's does. **Which renderer runs
  is settled in-process** (the `Log Loaded Modules` probe), not from the disk image.
- Strings: PSP-era paths (`disc0:/PSP_GAME/USRDIR/...`, `ms0:/PSP/SAVEDATA/ULUS10509...`,
  `pspnet_*`), namespaces `MGK::` (Render, GEngine::Shader, ScreenCapture), `GFX::` (Render,
  DXShader, DXPgmShader, DXGeometryShader, BaseShader), `NHT::KeyConfig`; `"../mgspw_savedata_win"`
  (saves beside the install; confirmed created on first run); `"invalid window size"`,
  `"resolution"`, `EnumDisplaySettingsA`, `GetWindowPlacement`, `SetWindowPos`. None of the
  MGS4 engine's vocabulary (`render.*` keys, scalability, bgfx, la2).
- Data: `SHADER\x64\` holds 11 files: `.cso` (DX bytecode), `.vpo`/`.fpo` (PSP vertex and
  fragment programs) and one `.XMD`. `MLG\` and `EXLANG\` (per-language `data`, `disc0_rel`,
  `Text`), `Text\*.txp`, `FONT\*.xpr`, `loading\*.txp`, `ms0\EU\DLC*`.
- Launcher: Unity, `launcher\launcher.exe`, like MGS4's.

## In-process (Measured, 2026-09-12, first Lab boot of MGSPWEnabler, probes on)

- **DirectX 11.** Modules at init: `d3d11.dll`, `dxgi.dll`, `d3dcompiler_47.dll`, `winmm.dll`,
  `steam_api64.dll`; the same set 20 seconds later at the title, with no `d3d12.dll` and no
  `D3D12Core.dll`. The launcher's Agility runtime folder is the launcher's own. One run, on
  this machine, with PatriotFix's launcher skip passing `-resolution 0 -upscale 0`; to be
  confirmed once with the launcher's own start, but the disk image's "no d3d12 import" now
  agrees with the process.
- **`.text` is readable when the ASI initialises**: entropy 5.81 bits per byte at
  `InitializeASI`, so the DRM has decrypted the code before the loader runs its plugins and
  pattern scans can run immediately (the MGS4 build had to poll for its signatures).
- **The command line the game received**: `-region eu -lan en -selfregion EU -resolution 0
  -upscale 0 -movie 0 -launcherpath launcher.exe -ctrltype PS5 -launcherroot "<install>\launcher"`.
  That was PatriotFix's launcher skip (its settings: Internal Resolution (PW) = Original,
  Skip Launcher = true) starting the game directly; `-ctrltype PS5` is a value the PatriotFix
  source does not list, so the launcher's own vocabulary is wider than KBD/XBOX/PS4.
- **Load order is alphabetical**: at init `MGSPatriotFix.asi` was already loaded and
  `MGSPWResolutionUnlocked.asi` was not (it was 20 s later). Anything of ours that must run
  after Afevis's patch cannot rely on init order; ours runs before his.
- Loader chain, root (`<install>`), log path (`logs\MGSPWEnabler_Game.log`), the lab marker
  protocol and the settings file: all as designed, first try.

## The frame at the title (Measured, 2026-09-12, Lab draw census, 3440x1440 window, Afevis's mod present)

The census (`pw\src\features\draw_census.cpp`, Lab only) hooks the DirectX 11 device,
swap chain and both context classes and logs every draw of a frame with its target, viewport,
shaders, first vertex constant buffer, bound texture, the vertex upload that preceded it and
the game-code callers.

- **The game records on deferred contexts.** Four deferred contexts are created at start-up;
  the immediate context draws nothing once the game is up. Three command lists a frame are
  submitted from `+17C59` (`ExecuteCommandList`), 60 frames a second at the title. Hooks that
  watch only the immediate context see zero draws.
- **Hook the context classes by vtable entry, not inline.** An inline (trampoline) hook on the
  deferred context's `Map` corrupted the driver's mapping bookkeeping: crash in
  `nvwgf2umx.dll` under `CResource::Map`, four seconds after the device, from the UI vertex
  upload (dump `METAL GEAR SOLID PEACE WALKER.exe.20916.dmp`). Replacing the entries of the two
  class vtables in `d3d11.dll` (`VirtualProtect` on the table) is stable across three boots.
- **One title frame is 30 draws, all to the 3440x1440 back buffer with a full viewport and
  scissor.** No smaller render target and no 480x272 (or multiple) target exists at the title:
  the UI is drawn straight into the output-sized target. Two full-screen passes bracket it
  (a 3440x1440 R32G32B32A32 target from `+5D202`, a copy from `+5CC84`, the last from `+56190`).
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
- **Cutscenes and character cards are pillarboxed by the game**: the mission intro (real-time
  Snake and soldiers) and the "HUEY" card render as a 16:9 picture with black bars each side
  at 21:9, while gameplay fills the window. Same class as MGS4's cutscene bands; the aspect
  work must leave those alone and only widen HUD and overlays.
- **Route timing**: the title starts its attract video if nothing is pressed for a while, so
  the replay must skip ahead (`bias_run.ps1 -SkipMs 15000`, since the replay clock starts
  after ASI init, later than the recording's); a late first Enter lands in the video and the
  rest of the route goes astray (run pw_hud2).
- **Our Lab hooks do not move the background**: the Release ASI (no census) and the Lab ASI
  produced the same Mission Selector capture, both at 6880x2880 through Afevis's ini. The
  user reports the map background offset; not attributed yet.

## How the launcher drives the game (Reported: MGSPatriotFix source, 2026-09-12)

The launcher starts the exe with `-region <r> -lan <l> -selfregion EU -resolution <0|1>
-upscale <0..3> -movie <0|1> -launcherpath launcher.exe -ctrltype KBD|XBOX|PS4 -launcherroot
"<path>"`; `-resolution` is Original or FHD internal rendering, `-upscale` Original, FHD, WQHD
or 4K output, `-movie` original or remastered FMVs. The launcher's choices persist as
`launcher_sv` under the save folder. So the game has a two-step internal-plus-upscale pipeline
of its own; the `Log Command Line` probe records what a given run received.

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

## Open questions (Phase 0 answers these)

- DirectX 11 or 12: answered (11, four runs; the DirectX 11 swap chain presents).
- Where the persisted display settings live and how `"invalid window size"` is reached.
- The internal render size for 3D (the title has no 3D): the UI is drawn at output size in
  canvas space (measured above); what the world renders into is the gameplay census.
- The shader set: 11 `.cso` against the runtime `Create*Shader` counts; what the `.vpo`/`.fpo`
  become.
- The perspective builder. The UI ortho is measured (480x272 over the whole viewport); whether
  the HUD looks stretched at 21:9 awaits the user's read of the title capture, and whether
  Afevis's HUD fixups alter the vertices or the ortho awaits a census with his ASI removed.
