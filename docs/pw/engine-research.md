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

- DirectX 11 or 12: answered above (11, one run); which device presents is the next probe.
- Where the persisted display settings live and how `"invalid window size"` is reached.
- The internal render size and where the upscale happens (3D at a 480x272 multiple then
  blitted, 3D at output size with the UI in canvas space, or a fixed HD target).
- The shader set: 11 `.cso` against the runtime `Create*Shader` counts; what the `.vpo`/`.fpo`
  become.
- The perspective builder and the UI ortho (480/272 constants), and whether the HUD is scaled
  uniformly or stretched at a wide window.
