# The Peace Walker lab

How research runs are done against Peace Walker: what the Lab build of `MGSPWEnabler.asi`
records, how a run is driven without a hand on the keyboard, and how the numbers it produces
turn into tables. The MGS4 equivalent is `docs/features/tooling.md`; the two share the same
shape (a Lab configuration, a marker file, a boot script, live commands, a census) and differ
where the engines differ. Nothing here ships: a Release build has none of it.

## 1. The Lab build and the marker

`MGSPWEnabler.vcxproj` has two configurations. **Release** is what ships. **Lab** is Release
plus the research probes (`pw\src\features\probe.cpp`, `draw_census.cpp`), compiled only under
`MGS4E_LAB` (the macro keeps its historic name; `shared\lab.hpp` keys on it). A Lab ASI reads
its settings from `MGSPWEnabler.lab.settings` only when the marker file `MGSPWEnabler.lab`
exists beside it, and it deletes the marker once read, so a lab boot is one boot: the next
start is back on the user's own `MGSPWEnabler.settings`. The log line
`LAB MODE: research instrumentation enabled for this boot only.` is the proof that the run
used the lab file; `boot.ps1` warns when it is missing.

Lab keys (all under `[Graphics]`; a Lab build reads them from `MGSPWEnabler.settings` too, so the
tool's Peace Walker window sets them on the "Rendering (Lab)" and "Lab" tabs; the lab file with
the marker still wins for a research boot):

| Key | What |
| --- | --- |
| `Log Loaded Modules` | the module list at init and again 20 s later (settled DirectX 11 versus 12) |
| `Log Decrypt Timing` | entropy of `.text` at init (readable at once on Peace Walker) |
| `Log Command Line` | the arguments the launcher passed |
| `Draw Census At (s)` | seconds after init to log a census automatically (0: only on command) |
| `Draw Census Frames` | frames per census; keep it 2, see section 4 |
| `Live Commands` | poll `logs\MGSPWEnabler_live.txt` (in the game folder) for commands (section 5) |

## 2. The harness (`C:\mgspf_tools\pw\`)

Every script dot-sources `game.ps1`, which holds the paths and names (the process name has
spaces and is always quoted). The scripts are copies of the MGS4 ones with the constants
swapped, plus what Peace Walker needed:

| Script | What |
| --- | --- |
| `boot.ps1 -Deploy -LabConfig -KeepOpen -WaitSeconds n` | copies `bin\<cfg>\MGSPWEnabler.asi` to `mgspw\scripts`, drops the marker, deletes our old log, starts the game through Steam, waits for `Initialisation complete`, captures, closes unless `-KeepOpen` |
| `close_game.ps1`, `shot.ps1` | tear-down and a window capture |
| `keylog.ps1 -Out routes\x.csv` | records key transitions with timing while the user plays a route; stops on End or when the game exits (Alt+F4 included); the first line stamps the absolute start |
| `replay.ps1 -Route routes\x.csv -SkipMs a -StopMs b` | replays a recorded route into the game window; `-StopMs` ends it early, `-SkipMs` starts its clock later in the recording |
| `census_ticker.ps1` | requests a two-frame census every 15 s while the game runs (long manual sessions) |
| `bias_run.ps1 -OutDir d -StopMs n -Commands ...` | one live-editing run end to end: boot, replay, capture, commands, census, capture, close, log copy |
| `textdump.ps1` | reads the decrypted `.text` and `.rdata` from the running process |

**Routes.** There is no stage automation for Peace Walker; a route is a recording the user
made once with `keylog.ps1`. `routes\to_mission.csv` goes from the title into the first
mission (Enter presses from 34.6 s, mission start at 84.1 s, gameplay from ~105 s).
`-StopMs 58000` lands on the Mission Selector; `-StopMs 108000` lands in the mission. Two
timing rules learned the hard way: the replay clock starts after the ASI reports init, which is
later than the recording's clock, so the presses must skip ahead (`-SkipMs 15000`) or the
title's attract video starts first and the whole route goes astray; and the mission has a
timer, after which it fails (restart without the menu is possible; not in the route yet).

## 3. The draw census

`pw\src\features\draw_census.cpp` is the instrument. Peace Walker renders through DirectX 11
with a twist the MGS4 lab never met: the game records its frames on **deferred contexts**
(four of them) and replays them with `ExecuteCommandList` on the immediate context, three
command lists a frame. Hooks on the immediate context alone see zero draws. So the census
hooks the DirectX 11 entry points (`D3D11CreateDevice`, the DXGI factory, the swap chain's
`Present`) and then both context classes, keeping its state (viewport, scissor, target, shaders,
constant buffers, bound texture, last vertex upload) per context.

The context methods are hooked by **replacing entries in the two class vtables** inside
`d3d11.dll` (`VirtualProtect` on the table, originals kept per class). An inline trampoline on
the deferred context's `Map` corrupted the driver's mapping bookkeeping and crashed the game
in `nvwgf2umx.dll` four seconds after the device came up; the vtable swap leaves the original
functions untouched and has been stable since.

A census logs, for every draw of N frames: the frame number, the context and draw index, the
call (`Draw`, `DrawIndexed`, ...), primitive count and topology, viewport and scissor, the
render target (size, format, object), the bound texture (size, format, object), the vertex and
pixel shader by bytecode hash, the vertex-shader constant buffers (slot, size, the latest
upload's floats, slot 0 in full), the game-code callers (a stack walk kept to frames in the
game image) and, when a vertex upload preceded the draw on that context, its size, decoded
head and callers. The first draws of a census frame also carry the call order of the
context methods before them.

Shader identity is a hash of the bytecode at `CreateVertexShader` / `CreatePixelShader`.
Texture and target objects change address every boot; sizes and formats do not, so tables
key on those.

## 4. Reading a census

`census_table.py <log> <frame>` (private module, `pw\tools`) prints one frame as an element
table; what the columns mean is described there.

Two rules:

- **Always census two frames.** The deferred recording straddles `Present`, so the first
  censused frame ends with 0 or 1 draws and the second carries the whole picture. One-frame
  censuses (the early ticker) were empty.
- **Match the capture to the census by geometry, not by assumption.** The Mission Selector was
  identified because its list rows, highlight, columns and tab icons in the table matched the
  capture line for line; the user's read of every capture is still the final word.

What the census established about the engine is in `engine-research.md`.

## 5. Live commands

With `Live Commands` on, the ASI polls `logs\MGSPWEnabler_live.txt` in the game folder four
times a second, runs each line and truncates the file (the harness's `$LiveFile`):

| Command | What |
| --- | --- |
| `census <frames>` | log the next `<frames>` frames |
| `bias ...`, `tbias ...` | the live-editing commands; they come from the private ultrawide module (`external\ultrawide\pw`) through `pw\src\features\ui_bias.hpp` and are reported as unknown when it is absent |

The census frame line reports how many uploads the module's biases have touched, so a
command that matched nothing is visible in the log, not just on screen.

## 6. A run, start to finish

```
bias_run.ps1 -OutDir <dir> -StopMs 108000 -Commands '<live command>','<live command>'
```

boots the Lab ASI with the marker, replays the route into the mission, captures `before.png`,
writes the commands, requests a two-frame census, captures `after.png`, closes the game and
copies the log next to the captures. The user reads the two captures; the log's applied
counts say what the ASI did. That loop, capture in hand, is how the HUD groups were found and
confirmed in one session.

## 7. What is not here

No screen detectors (MGS4's colour-probe detectors have no Peace Walker equivalent yet; the
census table is the detector when one is needed), no stage automation, and no PIX: the census
gives the game-side numbers a GPU capture would not. In-game hotkeys do exist now, for the
frame-pacing work: F11 a census, F10 the missed-tick detail, F9 the thread sampler
(`frame-pacing.md`).

## Latched lab mode (2026-09-17)

The one-boot marker (`MGSPWEnabler.lab`, dropped by the harness, consumed on boot, ignored after
fifteen minutes) is unchanged. The **latch** is its persistent form: `MGSPWEnabler.lab.latched`
beside the settings files, written and removed only by the Config Tool, honoured by a Lab plugin on
every boot and never seen by a Release plugin, since the check lives inside the Lab-only block.

The tool detects a lab setup without running anything: it reads the deployed
`mgspw\scripts\MGSPWEnabler.asi` for the literal `LAB MODE: research instrumentation`, the same
string the packager refuses a build on, so the two cannot drift. When it is present, the icon on
the About page becomes the latch. Clicking it creates `MGSPWEnabler.lab.settings` from the shipped
settings if missing (never the reverse), writes the latch, and reopens the window on the lab file.
While latched the title reads `LAB | ...`, the status bar names the lab file, and **the tool edits
the lab file**, so the game and the tool agree on one file. **Revert to shipped** removes the latch
only; the lab settings and their knobs are kept. Unsaved changes prompt before either action.

A latched Lab boot logs `LAB MODE: research instrumentation latched by the Config Tool; every boot
reads MGSPWEnabler.lab.settings until Revert to shipped is pressed.` The MGS4 plugin does not have
the latch yet (user's call: later); the tool side is written once for both games.

## RenderDoc captures (2026-09-17)

For identifying UI draws, a RenderDoc frame beats the census: the user clicks the pixel
(pixel history) and reads an event id; `C:\mgspf_tools\pw\rdoc\rdc_export.ps1 -Capture x.rdc`
turns the capture into one TSV row per draw (event id, UI-ortho flag, translation row, texture
on pixel slot 0, quad rectangle, viewport, target; ~25 s for an 8k-draw frame), and
`compare_frames.py` diffs the HUD groups across captures. The scripts run inside
`qrenderdoc.exe --python` (Windows ships no standalone module).

Launch for capture with `renderdoccmd capture --working-dir mgspw --opt-hook-children <exe>`
plus the launcher's arguments (`docs/pw/engine-research.md`). One constraint found the first evening: under RenderDoc the game receives wrapper contexts of
one class, so the immediate and deferred contexts share a vtable, and both the Release render
hooks and the Lab census patched it twice, recording our own `Hooked_Map` as the original and
recursing until the stack was gone (a clean-looking exit 1.4 s after device creation; found with
cdb attached on the log line "Initialisation complete", exception c00000fd in `Hooked_Map`).
Fixed 2026-09-17 (the second install aliases the first when the vtable is the same); either
build now runs under RenderDoc, so wide-canvas captures with the fix loaded are possible.
