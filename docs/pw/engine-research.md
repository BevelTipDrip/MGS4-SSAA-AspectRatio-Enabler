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

The census (`pw\srceatures\draw_census.cpp`, Lab only) hooks the DirectX 11 device,
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
  earlier. The MGS4 method (owner attribution by unwinding to the element's builder) has no
  purchase here; the identity of a quad is its **texture** (object, size, format) plus its
  **canvas rectangle**, and its order within the frame.
- **The title's element table** (draw order, texture, canvas rect):
  1 backdrop 512x512 `fmt71` (BC1) at (-256,-152)-(256,360); 4 horizontal strips and 4
  vertical strips of a second 512x512 BC1 tiling the canvas (the colour bars); 3 untextured
  half-width bands (0,-66)-(240,62) stepping one unit (the scan effect); a 256x256 BC1 and a
  256x128 `fmt77` (BC3) overlay at (-242,-138)-(242,138) (vignette); the 1024x128 BC3 line at
  (-128,116)-(128,132) (the copyright); the 2048x1152 BC3 art at (-240,-156)-(240,116) (the
  title); glyph runs from a 512x512 BC3 font atlas at y 58..112 (14, 8, 9, 6, 6 glyphs: "PRESS
  ? BUTTON" and four more runs to be identified); the 512x512 BC3 button icon at
  (-9.9,71)-(3.3,84); a 256x128 BC3 quad at (-232,72)-(-70,85) with colour `ff0a0a80`; and
  1024 tiny quads from a 2048x512 BC3 (the noise overlay). Texture object addresses change per
  boot; sizes and formats do not.
- **Live editing will be per quad**: the vertex upload is a `Map` with discard on a deferred
  context, so a bias keyed on (texture size and format, rectangle) can be applied to the
  mapped vertices at `Unmap`, before the draw is recorded. That is the Peace Walker equivalent
  of the MGS4 pool-append hook.

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
