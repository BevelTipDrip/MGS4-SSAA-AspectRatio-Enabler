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

Lab keys (all under `[Graphics]` in the lab settings file):

| Key | What |
| --- | --- |
| `Log Loaded Modules` | the module list at init and again 20 s later (settled DirectX 11 versus 12) |
| `Log Decrypt Timing` | entropy of `.text` at init (readable at once on Peace Walker) |
| `Log Command Line` | the arguments the launcher passed |
| `Draw Census At (s)` | seconds after init to log a census automatically (0: only on command) |
| `Draw Census Frames` | frames per census; keep it 2, see section 4 |
| `Live Commands` | poll `C:\mgspf_tools\pw\live.txt` for commands (section 5) |

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
game image) and, when a vertex upload preceded the draw on that context, its size, the decoded
canvas rectangle and colour, and its own callers. The first draws of a census frame also carry
the **call order** on their context (`VSSetShader > PSSetShader > VSSetCB > SRV0 > Unmap cb >
Unmap vb > Draw`), which is what told us where an edit can be applied.

Shader identity is a hash of the bytecode at `CreateVertexShader` / `CreatePixelShader`.
Texture and target objects change address every boot; sizes and formats do not, so tables
key on those.

## 4. Reading a census

`scratchpad\census_table.py <log> <frame>` prints one frame as an element table: the UI draws
(those whose constant buffer starts with the 480x272 ortho) in order with texture, world
translation and rectangle, then the non-UI draws grouped by target, texture and shaders.

Two rules:

- **Always census two frames.** The deferred recording straddles `Present`, so the first
  censused frame ends with 0 or 1 draws and the second carries the whole picture. One-frame
  censuses (the early ticker) were empty.
- **Match the capture to the census by geometry, not by assumption.** The Mission Selector was
  identified because its list rows, highlight, columns and tab icons in the table matched the
  capture line for line; the user's read of every capture is still the final word.

What the census established about the engine is in `engine-research.md`.

## 5. Live commands

With `Live Commands` on, the ASI polls `C:\mgspf_tools\pw\live.txt` four times a second,
runs each line and truncates the file:

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
census table is the detector when one is needed), no stage automation, no in-game hotkey (a
census on a key press is the planned tool for the lens-flare bug), and no PIX: the census gives
the game-side numbers a GPU capture would not.
