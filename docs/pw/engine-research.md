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
shaders, constant buffers, bound texture and the game-code callers.

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
- How the UI itself is drawn (canvas, constants, vertex layout, identity of an element) is
  research for the aspect work and lives in the private ultrawide module
  (`external\ultrawide\pw\docs\`).

## Gameplay (Measured, 2026-09-12, route replay, 6880x2880 window)

- **Gameplay renders straight into the output-sized target too.** A mission frame is ~8,800
  draws, all on the 6880x2880 back buffer (plus a 192x192 blur chain). There is no separate
  internal 3D target to resize: with Afevis's patches the world is drawn at window size, which
  is why his supersampling has to be a bigger window.
- **Cutscenes and character cards are pillarboxed by the game**: the mission intro (real-time
  Snake and soldiers) and the "HUEY" card render as a 16:9 picture with black bars each side
  at 21:9, while gameplay fills the window.
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

Afevis's patch overwrites the game's two preset resolution tables (the fullscreen 720p/1080p/
1440p/4K slots the launcher's `-resolution` picks from, and the windowed table) with a custom
size, makes the internal render target the output size at nine sites, and forces the one
float that scales the game's units to pixels (output height over the native height). It does
not touch window creation, style, position or DXGI: window mode stays the launcher's
business. "Supersampling" in his ini is only a larger custom size, so the swap chain and every
target follow it (a 6880x2880 window on a 3440x1440 desktop, measured); his ini parser rejects
values with thousands separators (`6,880` reads as 0 and falls back to the desktop size). What
the patch does for the HUD and the aspect is in the private module.

The game's own window, display-mode, settings and swap-chain plumbing was read from a live
dump on 2026-09-13; the notes are in the private module.

## Open questions (Phase 0 answers these)

- DirectX 11 or 12: answered (11, four runs; the DirectX 11 swap chain presents).
- Where the persisted display settings live and how `"invalid window size"` is reached.
- The internal render size for 3D: answered (the world is drawn into the window-sized target).
- The shader set: 11 `.cso` against the runtime `Create*Shader` counts; what the `.vpo`/`.fpo`
  become.
- The perspective builder (3D camera).
