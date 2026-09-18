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

## Hidden developer menu: searched, none in the executable (2026-09-17)

**Method.** The live-decrypted dumps (`C:\mgspf_tools\pw\pw_text.bin`, `pw_rdata.bin`, `pw_data.bin`;
the rdata dump is fully decrypted, 0 of 81 blocks at encryption-level entropy) swept for ASCII,
UTF-16 and Shift-JIS strings: debug, test, develop, cheat, muteki, stage select, god, noclip,
console; RTTI class names; PSP debug facilities; launch switches; input chords.

**Result: no debug menu is named anywhere in the executable.** The only plain menu identifiers are
the shipped ones, in a table beside `set_menu_info_%02d` / `set_menu_system_%02d`: `staff develop
delivery cyberval metal info_myouter info_player info_log trade recruit mission_select option
key_config save game_exit`.

**False leads, so they are not chased again.** `KONAMITESTPLAY`, `DEVIL` and `PWALKER` are entries
in the banned-name filter (RVA `+D57698` region: player/base names the game refuses, in many
languages). `MGSPW_VOCALOID` is the dead online DLC download service. `MGSPW_WIN32` is the window
class. The `KEY_*` table is the keyboard vocabulary; nothing binds an F-key to anything hidden.
Launch switches beyond the documented `-region -lan -selfregion -resolution -upscale -movie
-launcherpath`: `-ctrltype`, `-launcherroot` (launcher plumbing), the rest are noise.

**What IS there: script argument lookup, not a toggle store (corrected 2026-09-17 evening).**
`+11F8D0` is the MGS strcode hash (`h = ((h >> 19) | (h << 5)) + c`, masked to 24 bits after every
step, empty string -> 1; disassembly confirmed). `+A52E0` finds a named argument on the script
command currently being executed. Of its 1325 call sites, 1119 pass a compile-time constant in
`ecx`: 662 distinct codes (`C:\mgspf_tools\pw\pw_var_codes.txt`, one line per code with its
resolved name where known and every call site). Resolving them against the hash with controlled
candidate pools (expected chance hits stated per pool: brute force to 3 characters ~2, identifier
strings present in the binary ~0.6, a curated debug vocabulary ~1.5) gives ordinary per-object
script arguments: `pos rot scale size color alpha angle model model_name item item_id weapon
mission mission_id region region_id proc eft_file_name param flag mode life hp enemy player event
camera collision pause rank option slot`, the pad-direction pairs `UD UL UR LL LR ...`, and the
single letters `r g b a` (whose codes are their ASCII values, which is what validated the scheme).
`debug` (x3, `+1663A1 +20F949 +2EC55A`) and `debug_flag` (`+2F98D3`) exist, but each is read as an
argument of one script command and stored into that object's own field (a bit at `+0x10d0`, an int
at `+0x72c`), i.e. a per-object switch that a stage script may or may not set. There is no global
"debug mode" variable and no menu behind these.

The heap region copied from the running game (`region.bin`, 2 MB at the cursors the +10A63F0
context pointed at) contained none of the common codes (`pos`, `rot`, `model`, `flag`, `proc`,
`color`, any byte order): that allocation was not the script argument store, so the earlier record
layout guess (`C4 <code> 5x`) is void. Not pursued further: the arguments live inside the stage
script data, and enumerating them would only list what stage scripts set, not a hidden menu.

**The scripts themselves (2026-09-17, late).** In the sibling games the dev menu lives in the
stage scripts, not the executable (MGS2 PC: swap `scenario.gcx`; MGS3 MC: a save that makes the
script open it), so the exe-only search above could not have seen one. All 92 Peace Walker
stage scripts were extracted from a copy of `009645fa.PDT` (= `STAGEDAT.PDT`; pipeline and
gotchas in `C:\mgspf_tools\pw\gcl\README.md`) and scanned at the token level: 408k command
tokens, 399k strcode arguments (22,920 distinct), 46k string literals including the developers'
EUC-JP `print` comments. Commands resolve to `if command switch load print return trap`; no
argument hash resolves to any debug/dev/cheat/menu name; `debug` occurs once, as a per-object
argument in `w04s06b`; every `dev` literal is Xbox "device change" plumbing; the Japanese hits
are pause-menu restart messages. So the script layer holds no developer menu either.

One curiosity: `nht_sound_test` is a complete stage in the shipped STAGEDAT (script, qar, rlc)
that no other script references by name or hash: an orphan sound-test room. Whether the port
can still run it is untested; forcing it would mean substituting the stage name at the
engine's stage-load call from a Lab build, which is a separate experiment.

**Conclusion.** No evidence of a hidden developer menu in Peace Walker's executable: no menu id,
no key binding, no launch switch, no global debug variable. Whatever debug tooling existed at
Kojima Productions was compiled out of the shipped PSP build and the Master Collection port did
not add any back.

## Game update 2026-09-18 and the move to signatures

Steam replaced the executable at 00:40 on 2026-09-18 (build id 25049520 -> 25294658; the new exe
is linked 2026-09-13 04:37:52 UTC, `.text` raw size 0x98B800 -> 0x98BA00). The code is the same
apart from an insertion around `+2A000..+4A000`: nothing moves below `+2A000`, everything from
about `+3F000` to `+5C000` moves +0x150, everything from `+76000` on moves +0x180. `.rdata` and
`.data` keep their layout (the output table, the settings array, the vblank slots and the
display-object pointer are at the same RVAs, confirmed through the new code's displacements).
Every one of the 52 code sites the two ASIs use is byte-identical at its shifted address; the
old fixed RVAs simply pointed at the wrong bytes, the byte checks refused them, and the game
booted to a white screen with nothing patched (PatriotFix was in the same state).

Since then the sites are found by **signature** (`pw/src/core/sites.hpp`): a masked byte pattern
that starts at an instruction boundary, wildcards rel32 branch targets and RIP-relative
displacements, is scanned around the last known address first and over the whole `.text` as a
fallback, and is accepted only when it hits exactly once. Immediates inside an instruction carry
an offset from the pattern start. The generator (`C:\mgspf_tools\pw\siggen.py`) anchors on
boundaries from the exe's `.pdata` and the nearest padding run (decoding backwards guessed
wrong twice), and proves each pattern unique in both builds' dumps before it is used; the table
is `signatures.txt` beside it. The log's first PW line names the build (link timestamp) and any
site that moved from its hint is logged with the distance.

**Procedure for the next update:** launch the game once with the ASIs off, `textdump.ps1` at
the title (update `$Sections` from the new PE header), keep the previous dump as
`pw_text_old_<buildid>.bin`, run `siggen.py`, read `signatures.txt`: sites that resolve need
nothing (the hints in the sources can be refreshed at leisure); a NOT UNIQUE or missing site is
a real code change and needs reading.
