# MGS4 crosshair wrap — investigation state

Notes from the search for the code that produces the wrapped UI coordinate. **The search is
finished** - section 0a has the cause and the fix. What follows is kept as the record of how it
was narrowed down, because several of its intermediate conclusions were wrong in instructive ways
and the corrections are marked inline.

Companion to `mgs4-rendering-research.md`, which holds the settled background on the renderer.

**For current status, see [`features/`](features/README.md)** - the per-feature bug lists are
where active issues live. This file is the narrative of one investigation, kept for the
record; it is not a status page.

The investigation was done while this code lived inside a fork of MGSPatriotFix, so the file
names, settings file and log it mentions are that fork's (`src/fixes/…`, `MGSPatriotFix.settings`,
`MGSPatriotFix_Game.log`, its Config Tool). In PF Companion the same code is under
`asi/src/features/`, the research keys are read from `PFCompanion.lab.settings` in lab mode
(see [features/tooling.md](features/tooling.md)) and the log is `logs\PFCompanion_Game.log`.

**Status: SOLVED, AND FIXED AT THE SOURCE.** The producer is four `movsx` instructions that keep
only the low 16 bits of a 32-bit coordinate; patching them removes the limit entirely. See §0a.
The measurement history below stands and is what confirms the fix addresses the right value.

**Original status line:** SOLVED. The wrapped anchor has been read live out of the vertex stream and matches
int16 wraparound at 1/16 px exactly, on both axes, at two resolutions. See section 0. The sections
below are kept as the record of how it was narrowed down; several of their intermediate claims are
corrected in 4d-4f.

---

## 0a. The fix - patch the truncation itself

**The producer is four sign-extension instructions.** The engine holds the reticle's screen
coordinate in a 32-bit register and then keeps only the sign-extended low 16 bits:

| RVA | Original | Patched | Axis |
| --- | --- | --- | --- |
| `+0xE39816` | `movsx edx, cx` | `mov edx, ecx` + `nop` | X |
| `+0xE39830` | `movsx r8d, cx` | `mov r8d, ecx` + `nop` | Y |
| `+0xE398F1` | `movsx edx, cx` | `mov edx, ecx` + `nop` | Y |
| `+0xE3990C` | `movsx edx, cx` | `mov edx, ecx` + `nop` | X |

`movsx edx, cx` with `ecx = 61440` yields `edx = -4096` - **exactly the value measured out of the
vertex stream**, which is what confirms these are the right sites and not merely plausible ones.

**Correction:** an earlier version of this note claimed `+0xE3xxxx` was the same region as
`mgs4.exe+0xE628C` from our captured producer stack. That is wrong - `0xE628C` is ~943 KB into
the image and the patch sites are at ~14.9 MB, about 8 MB apart. Our write watch never got near
them, and the two lines of evidence do **not** meet in the way that claim implied. What does
corroborate the sites is the arithmetic alone: the value they produce is the value measured.

**Credit:** the four sites were identified by
[drbermejor/mgs4Ultra120](https://github.com/drbermejor/mgs4Ultra120)
(`src/reticle_truncation_patch.h`). Our contribution is the independent measurement that explains
*why* they are the right sites, and the verification below.

### Verified

Implemented in `PatchReticleTruncation()`, on by default, `[Graphics] Fix Reticle Truncation`.
It waits for `.text` to decrypt, refuses unless all four sites match their expected bytes, and
verifies after writing - so a game update produces a clean refusal rather than a corrupted patch.

Tested at a **7680x4320** internal buffer with the shader workaround **off**:

```
MGS4: Reticle truncation: +E39816 movsx edx, cx -> mov edx, ecx (X)   [and three more]
MGS4: Internal Resolution: render buffer 3840x2160 -> 7680x4320
CROSSHAIR PRESENT
```

That configuration has never rendered the reticle in this investigation. It does now.

### Why our approach could not have found this

Every search we ran looked for a **memory** operation, and this truncation is
**register-to-register**. `movsx edx, cx` reads the low half of `ecx` and sign-extends it into
`edx`; no int16 is ever stored, so:

- scanning for float-to-int16 stores, packed 32-to-16 stores, or 16-bit stores after a convert
  could not match it - there is no store;
- watching memory for writes could not match it - there is no write;
- the whole-process sweep for the pair `(-4096, -30976)` found it only where it had already been
  written *downstream*, which is why it was transient and why arming a watch on it kept losing
  the race.

The one hypothesis in §5 that was close in spirit - "16-bit arithmetic wrapping in the
computation" - scanned for `shl reg16, 4` and `imul reg16, ..., 16` feeding a store. It was
looking at register-level 16-bit arithmetic, but still expected a store, and it did not include
`movsx r32, r16`.

**What would have found it:** a disassembly sweep of `.text` for `movsx r32, r16`, filtered to
sites whose surrounding code touches the render size or the UI path. We already had the Zydis
sweep infrastructure for exactly this shape of search; we simply never pointed it at this
instruction.

### What this supersedes

The `Fix UI At High Resolutions` shader patch detects a wrapped anchor after the fact and adds a
wrap back. It works, but it is a heuristic, it needs a margin to tell a wrapped anchor from one
legitimately at a screen edge, and it caps the buffer at 6720 wide. Fixing the truncation removes
the cause instead, so **the `iMaxInternalBufferWidth` ceiling and the shader workaround should
both be revisited** now that the coordinate keeps full precision.

---

## 0. The root cause

**The reticle's screen anchor is held as a signed 16-bit value at 1/16 px, so it overflows once
the render buffer exceeds 4095 px wide - independent of window size.** The anchor has now been
read live out of the vertex stream and matches plain int16 wraparound exactly.

The field holds at most `32767 / 16 = 2047.94 px`. The reticle sits at the centre of the buffer,
i.e. `bufferWidth / 2`, so it overflows as soon as the buffer passes `4095.9` px wide. This is the
same 4095 the investigation has used from the beginning; what is new is that the wrapped value has
now been measured directly rather than inferred from a GPU capture.

### The proof

`v8`, the quad's NDC anchor, was read live from the vertex stream at both resolutions and compared
against what plain int16 wraparound predicts:

| Buffer | Axis | centre px | x16 | int16 | as px | predicted NDC | **measured `v8`** |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 3840x2160 | x | 1920 | 30720 | fits | 1920.0 | 0.0000 | **0.0000** |
| 3840x2160 | y | 1080 | 17280 | fits | 1080.0 | 0.0000 | **0.0002** |
| 7680x4320 | x | 3840 | 61440 | **wraps** to -4096 | -256.0 | -1.0667 | **-1.0666** |
| 7680x4320 | y | 2160 | 34560 | **wraps** to -30976 | -1936.0 | 1.8963 | **1.8962** |

Exact on both axes at both resolutions. It also reproduces the figure from the original PIX
capture at 7680x4320 - `(-1.07, +1.90)` - by a completely independent route, which is a useful
cross-check: the PIX number came from reading the shader's output, this one from reading the
vertex stream's input.

**Watch the buffer sizes.** These runs were on a 3840x2160 display, so "100%" means a 3840-wide
buffer and "200%" a 7680-wide one. The settings file's `Window Resolution (16:9)="1920x1080"` was
*not* in effect - `Window Aspect Ratio = "Use Game Setting"` leaves the override off, and the game
renders at the display resolution. Assuming a 1080p window here produces an arithmetic that fits
the same measurements with the wrong constants (32 units/px and a 2048 px threshold) and then
contradicts the fact that stock 4K works. Confirm the real size from the log line:

```
MGS4: Internal Resolution: render buffer 3840x2160 -> 7680x4320 (2.00x scale, 4.00x pixels).
Output stays at 3840x2160.
```

### How it was isolated

The reticle is five quads - a ring plus four corner brackets at `+-0.0391, +-0.0694` in the
1280x720 virtual basis. Clustering anchors by that shape finds exactly one match per run:

- **3840-wide buffer:** centre `v8 = (0.0, 0.0002)` - the NDC origin, i.e. screen centre. Correct.
- **7680-wide buffer:** centre `v8 = (-1.0666, 1.8962)` - far outside `[-1,1]`, so it rasterises
  off screen and silently disappears.

At 3840 **no** anchor in the entire 1600-sample capture falls outside NDC. At 7680 seven do, and
five of them are this cluster.

### Confirmed independent of window size

The decisive test, run afterwards: **1080p output with a 3840x2160 internal buffer**, UI fix off.

```
MGS4: Internal Resolution: render buffer 1920x1080 -> 3840x2160 (2.00x scale).
Output stays at 1920x1080.
```

**Crosshair renders correctly**, and 800 sampled anchors contain **zero** values outside the NDC
box. The full matrix:

| Output | Buffer | Anchors outside NDC | Crosshair |
| --- | --- | --- | --- |
| 3840x2160 | 3840x2160 | 0 / 1600 | present |
| 1920x1080 | 3840x2160 | 0 / 800 | present |
| 3840x2160 | 7680x4320 | 7 / 1600 | **absent** |

The same 3840 buffer is fine behind either a 4K or a 1080p window, and only the 7680 buffer
wraps. **Buffer width is the only variable that matters**, exactly as held from the start.

### Note on a wrong turn

An earlier pass here concluded the threshold was a 2048 px buffer, from assuming these runs used a
1080p window. That is wrong twice over: it contradicts stock MGS4 rendering correctly at native
4K, and it disagrees with the long-established 4095 figure. The arithmetic was right and the
inputs were wrong. The lesson is cheap to state - read the render size out of the log rather than
inferring it from the settings file, because the settings file's window resolution is frequently
not what the game is actually using.

## 0b. Hunting the producer - state

The root cause is known; what is not known is the code that writes the value. The tooling for
that is built but has not yet caught it.

**`ArmAnchorWatch`** sets a hardware write watch on the anchor's exact address, taken from the
input layout. This replaces `TryArmStagingWatch`, which searched staging buffers for the wrapped
coordinate as an *int16 pair* - a search that could never have succeeded, because by the time the
value reaches the vertex buffer it is a float in NDC. That explains the "never found" result
recorded in the eliminated-hypotheses table.

### Two traps found while setting this up

- **The internal resolution scale is clamped to 200%** in `Config::Read`
  (`std::clamp(percent, 100, 200)`). Setting 300 in the settings file silently yields 200, and
  the log reports the clamped value. This cost two runs that looked like the config was being
  ignored. It also means a buffer past 4095 px is unreachable from a 1080p output - the wrap can
  only be produced from a 4K output at 200%.
- **Screen capture and `SetForegroundWindow` fail entirely when the display is asleep**
  (`CopyFromScreen` throws "the handle is invalid"). The game keeps running and logging normally,
  so this presents as the automation mysteriously breaking. Wake the display first.

### Why the watch has not fired yet

Two problems, both in reading rather than watching:

1. **Staging-resolved reads are unreliable.** When `ResolveVertexData` fails and the read falls
   back to `TraceVertexToStaging`, the returned pointer is frequently the wrong copy: `v6` comes
   back as `(0,0)`, `v7` as `(0,1)`, and `v8.z` equal to `v8.x` - the signature of data shifted by
   whole float4s. In one run this produced **814 "wrapped" anchors out of 1614**, when the true
   count should be about five per frame.
2. **The wrap test is too loose.** `|v8| > 1.02` cannot distinguish a genuine wrap from a
   misaligned read, so the watch armed on a garbage address and nothing ever wrote to it.

**Next step:** validate the read before trusting it. A correct anchor has `v8.z == 1.0` exactly
(the shader writes `o0.w = 1.0` and the third component is consistently 1.0 in every clean sample)
and basis vectors of the form `v6 = (2/W, 0)`, `v7 = (0, -2/H)`. Requiring that shape before
treating a value as wrapped will reject the shifted reads, and the watch will then arm on the real
anchor. The known-good target to confirm against is `v8 = (-1.0666, 1.8962)` at a 7680-wide buffer.

### What the watch found

The write watch works, and produced two distinct producer stacks.

**The upload.** The vertex data reaches the GPU through a Map / memcpy / Unmap helper at
**`+7A8CD0`**, called twice from the UI submit function:

```
+7A57C8  mov r8,  [r15+0x24D35B8]   ; descriptor, count at [r8+0x10]
+7A57D4  imul rdx, rcx, 0x70        ; 112-byte records
+7A57F9  call +7A8CD0               ; upload B - this branch is never taken

+7A580A  mov rdx, [r15+0x24D35C0]   ; descriptor, count at [rdx+0x12]
+7A5815  imul rcx, rax, 0x78        ; 120-byte records
+7A583A  call +7A8CD0               ; upload A
```

Probing both by hooking the instruction that loads the source pointer (rather than guessing the
global's base, which is *not* the module base) shows **A carries only 2 records and B never
fires**. A's records hold an animating value stepping 1.49 -> 1.99 in 1/24 increments, so it is
some overlay, not the reticle.

**The fill.** Watching a value inside A's source caught the code that writes it:

```
VCRUNTIME140.dll memcpy <- mgs4.exe+0x68A938 <- +0x666485 <- +0x666729 <- +0xE628C <- +0xB8CA8
```

### Two gotchas worth keeping

- **A read-or-write watch fires on our own diagnostics.** The anchor dump memcpys the same
  address every frame, so every hit reported `MGSPatriotFix.asi` as the accessor. Anything the
  diagnostics themselves read must be watched **write-only** (DR7 type 01, not 11).
- **A still crosshair is never rewritten.** The wrapped coordinate is written once when the
  reticle is built. Holding aim - however long, and however much the view is moved - produces no
  further writes, so a watch armed during an aim finds the value already there and never fires.
  `boot_to_aim.ps1` now aims, releases and aims again for this reason.

### Where it stands

A whole-process sweep of private committed memory finds the wrapped pair
`(-4096, -30976)` - `(bufferW/2)*16` and `(bufferH/2)*16` after wrapping - in **exactly one
place** out of ~750 MB. That is a clean, unambiguous handle on the value.

But it is **transient**: present in two scans out of three. It is written into a scratch
structure while the reticle is built, converted to NDC, and gone. Scan-then-watch therefore races
it, and the watch has caught the initial write in none of the attempts so far.

**Next step:** stop trying to watch it and capture it instead. Scan every frame while aiming, and
on the first hit immediately dump the surrounding memory - say +-128 bytes, plus the containing
allocation's base and size from `VirtualQuery`. The structure's layout identifies what owns it,
and a known layout can be matched against code by its field offsets, which does not depend on
winning a race. The pair is unique in the address space, so a hit is never ambiguous.

### Why the shipped fix works

The UI shader patch corrects `v8` after the fact - the right value in the right place. That is why
it works, which had never actually been established before; it was empirically correct for reasons
nobody had traced.

---

## 1. What is actually broken

At internal render buffers wider than 4095, dynamic HUD elements — the aiming reticle, Solid Eye
overlays, the sniper scope — are drawn far off screen. Static UI (menus, HUD panels) is unaffected
at any resolution.

**Measured, from a PIX capture at 7680×4320:** the reticle's vertices leave the vertex shader at
NDC `(-1.07, +1.90)`, which is screen pixel `(-256, -1936)`. That is exactly 16-bit wraparound of
a 1/16px fixed-point value:

```
3840 * 16 = 61440  -> as int16 = -4096  -> -256px
2160 * 16 = 34560  -> as int16 = -30976 -> -1936px
```

So a coordinate of `bufferWidth/2` px, in 1/16px units, is being narrowed to 16 bits somewhere.
**This measurement is the anchor for everything else and was never contradicted** - it is what
identified the fix when the site was finally found.

**Corrected:** the original wording said the value is "held somewhere as 16-bit fixed point",
which implied an int16 field in memory. It is not stored at all. The narrowing happens
register-to-register, in `movsx edx, cx` - see section 0a. That wrong implication is precisely
what sent this investigation looking for stores and memory writes for weeks.

## 2. The shipped workaround

`Fix UI At High Resolutions` replaces both UI vertex shaders with copies that detect a wrapped
translate and add back one wrap (`2 * 4096 / dimension` in NDC). It is a heuristic, not an
inversion, and it caps the render buffer at 6720 wide. See `mgs4-rendering-research.md` §5.5–5.6
for the correction rules and why the ceiling exists. **This works and shipped in 0.2.4.**

**Superseded.** Section 0a fixes the truncation at its source, so the wrap never happens and
there is nothing for the shader to undo. The heuristic, its edge margin, and the 6720 ceiling all
exist only because of the wrap. Whether to keep the shader patch as a fallback for builds where
the conversions cannot be found is an open product question, not a technical one.

---

## 3. The rendering path (established by reading disassembly)

All RVAs are offsets into `mgs4.exe`. The image base at runtime has been `0x7FF7365E0000`, but
**do not hardcode it** — it is ASLR'd. Subtract `baseModule`.

### 3.1 Draw submission

```
+7A5D8A  mov rbx, [r14+0x15E98]      ; the D3D12 command list
+7A5D97  lea rcx, [r14+0x14FC8]      ; the UI draw-list container
+7A5D9E  call +79F2B0                ; draw loop, layer 0
+7A5DB3  call +79F2B0                ; draw loop, layer 1
```

`+79F2B0(container, commandList, layer)` walks an array of **104-byte (0x68) draw records**:

```
+79F387  mov  rdi, [rcx+r14*8+0x18]   ; record array for this layer
+79F39D  add  rdi, 0x50               ; rdi now points at record+0x50
+79F3C5  call [rax+0x130]             ; vtable slot 38: SetGraphicsRootConstantBufferView(2, [rdi])
+79F40B  call [rax+0x160]             ; vtable slot 44: IASetVertexBuffers(0, 5, rdi-0x50)
+79F429  call [r10+0x60]              ; vtable slot 12: DrawInstanced
+79F42D  add  rdi, 0x68               ; next record
```

**Record layout (104 bytes):**

| Offset | Size | Contents |
| --- | --- | --- |
| `0x00`–`0x4F` | 80 | **five** `D3D12_VERTEX_BUFFER_VIEW` (16 bytes each: GPU VA, size, stride) |
| `0x50` | 8 | constant buffer GPU address, bound to root parameter 2 |
| `0x58` | 4 | `VertexCountPerInstance` |
| `0x5C` | 4 | `InstanceCount` |
| `0x60` | 4 | `StartVertexLocation` |
| `0x64` | 4 | `StartInstanceLocation` |

### 3.2 Call stacks

UI quad draws (6 vertices, stride 12) report:

```
+0x79F42D <- +0x7A5DA3 <- +0x76628F <- +0x7663D7 <- +0x7664AE   crosshair / in-game HUD
+0x79F42D <- +0x7A70BE <- +0x76628F <- +0x7663D7 <- +0x7664AE   main menu UI
```

**These are different paths.** Sampling "the first N UI draws" from process start captures the
*menu*, not the crosshair. Sample while the aim button is held instead.

### 3.3 The draw list container

Lives at `+0x14FC8` in the renderer object. Enumerating every instruction addressing that offset
gives 11 references:

- `+798959` — construction (`call +79 83B0`)
- `+7A1799` — reserve, capacity `0x1000`
- `+7A5138` / `+7A514A` — the only read/write of the field itself
- `+7A5C65`, `+7A5D97`, `+7A5DAC`, `+7A6914`, `+7A70B2`, `+7A70CA`, `+7A7A67` — submit paths

**None of them appends records**, so the container is filled through a pointer passed in.

`+7A6914` is the closest to the emitter seen so far: it takes the container into `rsi` alongside
a cursor at `[rbp+0xA8]` bounded by `[rbp+0x168]`, with a 16-bit field at `[rbp+0x10]` and a
constant buffer address at `[rbp+0x130]`.

### 3.4 Memory

**The UI vertex pool is GPU-only.** Buffer creation logging reports:

```
Buffer created: 3145728 bytes in CUSTOM, not CPU accessible
```

3 MB, `D3D12_HEAP_TYPE_CUSTOM`, `CPUPageProperty = NOT_AVAILABLE`. The CPU cannot write it. It is
filled by `CopyBufferRegion` from CPU-visible staging.

**This engine allocates everything from `HEAP_TYPE_CUSTOM`**, never `HEAP_TYPE_UPLOAD`. Code that
filters on `HEAP_TYPE_UPLOAD` sees nothing at all — that bug silently invalidated many earlier
results (see §6).

Staging buffers are `CUSTOM, CPU write-combine` and *can* be mapped.

---

## 4. The critical recent finding

Tracing a UI quad's vertices back through the copy that filled the pool, and printing them:

```
Traced quad to staging at 0x214DA1D6608:
  (0.2,0.2) (0.2,127.8) (127.8,127.8) (0.2,0.2) (127.8,127.8) (127.8,0.2)
```

**The int16 `POSITION` holds a local unit quad, 0–128 px — not a screen coordinate.** Local
offsets of that size can never overflow int16.

So the long-held assumption that "the int16 position wraps" is **wrong**. The screen position
comes from the per-quad **translate**, which the shader reads as `v8` (`TEXCOORD7`) in the first
shader and `v7` in the second, from a *different vertex stream*. The wrapped value found
repeatedly in memory is that translate, not the position.

This is where the investigation stopped.

### Immediate next step

Only stream 0 has ever been captured, because `IASetVertexBuffers` was recorded into a
`thread_local` that a later single-view call overwrites, and empty slots are skipped. Fix the
capture so all five streams bound by `+79F40B` are recorded, then dump the anchor stream for a
UI quad while aiming. That shows the wrapped translate in its real location, and the hardware
write watch finally has a correct address to sit on.

Failing that, the per-draw constant buffer at record `+0x50` is the other place a translate could
come from.

---

## 4b. Automating the test loop - BUILT

Most of the cost in this investigation has been the human loop, not the analysis. Each hypothesis
takes one to three runs, and every run was a manual launch, load, aim and quit.

Implemented in `src/fixes/stage_automation.cpp`, behind `[Graphics] Stage Automation=1`.

### What it does

Resolves the engine's fast-load machinery by signature, hooks the game's per-frame tick for a
game-thread context, reads the stage registry, and loads a named stage on a hotkey (`F7` by
default, `Stage Automation Key`). `Stage Automation Stage` takes a comma-separated playlist and
each press advances through it and wraps, so a whole survey runs in one launch. An empty playlist
walks the registry with the `_D` entries filtered out.

`GetAsyncKeyState` is system-wide, so the hotkey can be driven by a script **without focusing the
game window** - which matters, because alt-tabbing pauses the game.

### Measured on this build (2026-08-30)

All signatures from MGS4-Debug matched our build unmodified, and every byte the pointer arithmetic
depends on validated:

| Symbol | RVA |
| --- | --- |
| `uiFrame` (per-frame tick) | `+661220` |
| `fastLoadStage` | `+57CC0` |
| `stageRequestHandler` | `+5C3A0` |
| `stageMapHead` | `+1D7C960` |
| `fastLoadStageId` | `+1D6E830` |
| `stageRequestFlags` | `+1D77A90` |
| `stageRequestBlocked` | `+5C180` |
| `setStageName` | `+5C9E0` |
| `finalizeStageRequest` | `+5C990` |
| `debugText` | `+660040` |

The registry holds **233 stages**, populated ~1.5s after our init - well before the title screen.
Names follow `sNNaMMl[_variant]`: `NN` is the act, `MM` the area within it, and a `_D` suffix marks
the cutscene variant. Acts run `s01`-`s05`; `s00` is the prologue and `s10`/`s20`/`s30` are the
trailing extras.

### The weapon survey

| Stage | Id | Result |
| --- | --- | --- |
| `s00a00l_D` | `0x166` | Long cutscene chain first, then Snake spawns **unarmed**. As a `_D` stage this is expected, and is why the filter exists |
| `s01a10l_1` | `0xEF` | **Weapon equipped.** This is the stage to build the loop on |

So the plan in the original form of this section worked: a stage that starts armed exists, and the
crosshair stays the signal. No substitute element was needed.

### The loop is closed - cold start to a captured verdict, no human input

`mgspf_tools\boot_to_aim.ps1` runs the whole thing in about 90 seconds and reports its own
verdict. No save file is involved: the stage request builds the session itself.

```
launch via steam://rungameid  ->  Enter (autosave notice)  ->  Enter (title)
  ->  F7 (ASI requests s01a10l_1)  ->  Enter (confirm, when the screen says to)
  ->  gameplay, AK-102 equipped  ->  hold right mouse  ->  screenshot  ->  verdict
```

Validated on matched runs of the identical scene: **100% renders the reticle, 200% does not**,
with everything else in the HUD unaffected. That is the investigation's signal, reproduced
automatically.

Four things had to be got right, each of which cost a run to find:

**Launch through Steam, never the wrapper directly.** Running `Launcher\launcher.exe` ourselves
gives `mgs4.exe` a process without Steam's environment, so its DRM stub asks Steam to relaunch and
exits with 53. Steam then shows a "Launch Game with custom arguments" prompt which is drawn
*inside* the Steam client window rather than as a top-level window - so it cannot be found by
window enumeration, and Enter does not activate its Continue button. `steam://rungameid/2492670`
sidesteps all of it: Steam starts the wrapper itself, the DRM stub is satisfied first time, and the
game is up in about two seconds with no prompt at all.

**Kill `launcher_original.exe` too.** Our bypass falls back to it when `CreateProcessW` on the game
fails, but that fallback can never work - Unity derives its data folder from the executable name,
so the renamed original looks for a `launcher_original_Data` that does not exist and puts up a
modal error instead. That modal keeps Steam's `Running` flag set, and while it is set Steam
silently ignores every later `steam://rungameid`, which presents as the launch simply not
happening. **The fallback is worth removing from the wrapper outright.**

**The fix truncates its log at startup**, so a byte offset taken before a run points past the end
of the new log and every wait times out having read nothing. Treat a file shorter than the mark as
a new session and reset to 0.

**Gate on the HUD, not on a timer or on the transition flag.** The confirmation prompt appears a
variable few seconds after the request, and pressing Enter early navigates whatever menu is still
up. `StageRequestBlocked` is no help either: the ASI's per-frame hook does not tick during a load,
so it never observes either edge and `ReportTransitionEdges` stays silent across the whole
transition. What does work is looking at the screen - see below.

### Reading the screen

Screen capture works even fullscreen, which removes the need for anyone to describe what the game
is doing. Two detectors, both validated against known screenshots before being trusted:

| Script | Question | Measured separation |
| --- | --- | --- |
| `in_game.ps1` | Is this playable gameplay? | Saturated orange in the top-left health bar. Menus, title and prompts all score **exactly 0**; gameplay scores **382-540** |
| `crosshair.ps1` | Is the reticle drawn? | Saturated orange near frame centre, requiring a large red-to-blue spread so Act 1's brown scenery does not match. Present **37-39**, absent **exactly 0** |

`in_game.ps1` is what makes the confirm step safe: Enter at the prompt loads the stage, but Enter
once in gameplay opens the pause menu, so the loop checks before every press and stops the moment
the HUD appears.

### What was straightforward

| Step | How |
| --- | --- |
| Launch | The launcher wrapper already reproduces the exact `CreateProcessW` the game requires; a script can start it directly. Steam must be running for the DRM |
| Load a stage | Write `FastLoadStageId` / `FastLoadMode`, `SetStageName("select")`, `StageRequestFlags \|= 0x11`, then call the finalizer - see §4c |
| Capture | The diagnostics already log themselves; nothing manual |
| Quit | Flush the log and exit the process once the data is in hand |

That reduces launch-to-data to one script invocation with no keyboard involved.

### The fragile part: getting into an aiming state - resolved

Aiming needs a weapon equipped, and a freshly loaded stage may start with nothing in hand.

**Do not substitute a different element for the crosshair.** The Solid Eye overlays wrap the same
way in principle, but their timing and consistency are unverified; the crosshair is the one
symptom that has behaved identically across every session. Keep the known-reliable signal.

The plan was to find a stage that starts armed rather than change the signal, and `s01a10l_1` is
one. So the only input still to synthesise is the aim button, and the ASI should confirm it saw UI
draws on the crosshair path (`+7A5DA3`, not the menu's `+7A70BE`) before trusting a run.

### Sketch

```
script: launch game
  ASI: wait for init -> request stage -> wait for load
  ASI: SendInput right mouse down (window focused)
  ASI: run the diagnostic, log, flush
  ASI: exit process
script: read log, decide next hypothesis, rebuild, repeat
```

## 4c. Where the fast-load machinery is

Established by [cipherxof/MGS4-Debug](https://github.com/cipherxof/MGS4-Debug), which reaches it
by signature. The globals are resolved from RIP-relative operands inside the fast-load function:

```cpp
FastLoadStageId  = ResolveRelativeAddress(fastLoadStage + 0x124);   // uint32
FastLoadMode     = FastLoadStageId + 1;
StageRequestFlags = ResolveRelativeAddress(stageRequestHandler + 0xaf) + 1;
FinalizeStageRequest = ResolveRelativeAddress(stageRequestHandler + 0xd3);
```

Their signatures (subject to game updates - re-derive rather than trust):

```
FastLoadStage:        40 57 41 56 41 57 48 83 EC 20 48 8B F9 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ??
                      FF 15 ?? ?? ?? ?? 4C 8B 7F 18 48 8B C7 4C 8B 77 10 49 83 FF 10 72 ??
StageRequestHandler:  48 83 EC 28 E8 ?? ?? ?? ?? 85 C0 0F 85 ?? ?? ?? ?? 48 89 5C 24 20
                      E8 ?? ?? ?? ?? 48 8B D8 80 38 00 0F 84 ?? ?? ?? ?? B1 6E E8 ?? ?? ?? ??
```

That project also shows the retail build contains a full **ImGui developer menu** (tabs for
DynamicResolution, Performance, Message, DofAdjust, Misc, plus stage-name and difficulty
overlays), and patches the dynamic resolution initialiser to disable it. The DynamicResolution
tab is the engine's own view of render sizes and may be worth reading directly.

## 4d. What the automated runs established (2026-08-30)

Three findings, all from matched 100%/200% runs of the identical scene and pose. The loop made
these cheap - each is one script invocation.

### Only one vertex stream is bound

**The "five streams" premise was wrong.** Across every captured reticle draw there is exactly one
bound stream: slot 0, stride 12, into the 3MB pool. Zero `stream 1:` lines, ever.

The old capture also had a bug that would have hidden extra streams anyway - it wrote `views[i]`
to `stream[i]` ignoring `startSlot`, so a call binding slots 1-4 landed in 0-3 and a second call
clobbered the first instead of accumulating. Fixed, and the answer is still one stream.

So the anchor cannot be "in a different stream". With one 12-byte stream, the per-quad translate
has to come from the constant buffer, and that is where it was found.

### The UI divisor scales with the internal render buffer

The per-draw root constant buffer's first two floats, at full precision:

| Internal buffer | `cb[0]` | `cb[1]` |
| --- | --- | --- |
| 1920x1080 (100%) | `0.00026041668` = **1/3840** | `0.000462962955` = **1/2160** |
| 3840x2160 (200%) | `0.00013020834` = **1/7680** | `0.000231481477` = **1/4320** |

So `cb[0] = 1/(2 x bufferWidth)` and `cb[1] = 1/(2 x bufferHeight)`, exactly, at both scales.

This is the mechanism that matters: **the UI coordinate space is not fixed - it grows with the
internal render buffer.** A full-screen quad measures 7680 units wide at 200% and 3840 at 100%,
i.e. two units per buffer pixel. Anything positioned proportionally to the screen therefore has
its coordinate doubled when the buffer doubles, which is what walks dynamic elements toward the
int16 limit as supersampling goes up.

Note this corrects the earlier framing that the UI lives in a fixed virtual 1280x720 space. The
static HUD survives high resolutions because its coordinates stay small, not because the space
is resolution-independent - it is not.

### The 100% control case had never actually run

`ApplyFixes` returns early when `fInternalResolutionScale <= 1.0 && !bOverrideWindowSize`, and
`StartAimWatchThread()` sits *after* that return. So at 100% the aim watch never started,
`g_AimHeld` stayed false, and the UI diagnostics never sampled anything.

**Every diagnostic session before this one was at elevated resolution.** The control case that
every "does this only happen when scaled?" comparison depends on was silently producing no data.
Fixed by starting the aim watch before the early return.

Two related traps found the same way:

- The log sink caps at 15MB. The staging trace was ungated, so it filled the entire cap during
  boot and loading, and by the time the crosshair was on screen nothing was being written at all -
  which presents as the diagnostics failing to fire. Now gated on `g_AimHeld`, same as the quad
  sampling.
- The `px:` dump read from `BufferLocation` without adding `startVertexLocation * stride`, so it
  printed vertex 0 of a shared 3MB pool - some unrelated quad - for any draw with a non-zero
  start. Fixed.

### The vertex layout - position is at offset 8, not offset 0

The 12-byte stride decodes as:

```
[int16 u][int16 v]  [uint8 r,g,b,a]  [int16 x][int16 y]
 offset 0            offset 4          offset 8
```

**Every earlier reading of "the position" was reading the texture coordinate.** Offset 0 is the
sprite's place in its atlas, which is why no draw ever appeared at screen centre, and why over a
thousand draws looked "reticle-sized" - atlas tiles are all small. Full-screen blits carry the
same numbers in both pairs, which is exactly what made the wrong field look right.

Proof: a 100% draw reads `... 80 07 38 04` at offset 8, i.e. `(1920, 1080)` - the exact centre of
the 3840x2160-unit screen space.

### The differential: nothing is drawn at the centre at 200%

Matched runs, sampling ~2000 UI draws per aim press, looking for small quads near screen centre:

| Run | Centre in UI units | Quads centred there (extent < 800) |
| --- | --- | --- |
| 100%, crosshair **present** | (1920, 1080) | **538**, mostly `x[1632,1760] y[654,974]` |
| 200%, crosshair **absent** | (3840, 2160) | **none at all** |

So at 200% the reticle is not merely mispositioned within view - nothing is placed near the centre
of the enlarged buffer, and the screenshot confirms it is not drawn anywhere on screen, upper-left
included.

Meanwhile full-screen quads *do* scale correctly: `x[0,3840] y[0,2160]` at 100% becomes
`x[0,7680] y[0,4320]` at 200%. And `x[1632,1760] y[654,974]` appears at **both** scales with
identical raw coordinates.

That is the shape of the bug: **the divisor scales with the buffer but some positions do not.**
An unscaled coordinate that landed at 44% of the screen at 100% lands at 22% at 200%. The prime
suspect is a position computed from the *window* size (which stays 1920x1080) while the space it
is interpreted in follows the *buffer*.

### Where the translate is not

Ruled out this session, by capturing them for the first time:

| Candidate | What it actually holds |
| --- | --- |
| A second vertex stream | Does not exist - only slot 0 is ever bound |
| Root 32-bit constants (slot 34/36) | Slot 1, four values, constant `(80, 0, 0, 0)` on every draw |
| Root CBV (slot 38) | Slot 2, per-draw, holds texture-size reciprocals (1/32, 1/128, 1/256 ...) |

There is no per-quad translate anywhere else: the screen position is in the vertex data itself, at
offset 8. That closes the question §4 left open.

### The aiming/idle differential

Built and working. Releasing the aim button arms a second sample of the same size, so one run
yields a matched AIM/IDLE pair from the same scene and camera; every draw is tagged and carries
its vertex colour. `boot_to_aim.ps1` waits out the idle sample before returning.

Results at 100% (2000 draws each):

| Draw | AIM | IDLE |
| --- | --- | --- |
| `x[1632,1760] y[654,974]` ext 128x320 | **512** | 60 |
| `x[-112,144] y[-492,20]` ext 256x512 | 275 | 203 |
| `x[-496,528] y[-416,608]` ext 1024x1024 | 94 | 11 |

The top row is the most aim-correlated draw in the game by a wide margin. It is probably **not**
the reticle: 128x320 units is a 64x160px box, far too tall for the ring, and its vertex colour
`0x2048A060` is not the reticle's orange.

**A better-shaped candidate:** four quads of 256x256, centred at `(+-32, +-32)` - a symmetric set
of four small boxes about a point, which is exactly the reticle's corner brackets. One of them is
aim-specific and has no counterpart at 200%.

### An unresolved contradiction - read this before continuing

Those four bracket quads sit around the **origin**, not screen centre. But the reticle plainly
renders at screen centre at 100%.

That cannot be reconciled with the conclusion above that the screen position is entirely in the
vertex data at offset 8 - if it were, these quads would draw in the top-left corner. So one of
the following is true, and which one should be settled before building on either:

1. There is a per-quad translate after all, applied somewhere still unfound, and these quads are
   the reticle drawn in local coordinates.
2. These four quads are not the reticle, and the resemblance to corner brackets is coincidence.

Note the patched UI vertex shader already assumes the first - it corrects an anchor in `v8`/`v7`,
a shader register whose source has never actually been traced back to a vertex stream or buffer.
With only one 12-byte stream bound, where that register is fed from is now an open question in its
own right, and answering it likely answers this one too.

**Settled by reading the shader.** See below.

## 4e. The reticle shader, read directly (2026-08-30)

The disassembly was already on disk at
`mgspf_tools\shaders\mgs4Shaders\c0f6fdb2-73bd444d-069170c9-a85fc8a1.asm`.

### What it computes

```
itof  r0.xy,  v1.xyxx                      // POSITION (int) -> float
mul   r0.xy,  r0.xyxx, v3.xyxx             // scale by TEXCOORD1.xy
mul   r0.yzw, r0.yyyy, v7.xxyz             // y * basis vector v7
mad   r0.xyz, r0.xxxx, v6.xyzx, r0.yzwy    // + x * basis vector v6
add   o0.xyz, r0.xyzx, v8.xyzx             // + v8
mov   o0.w,   l(1.000000)
```

`o0.w` is a literal `1.0`, so there is **no perspective divide**: the shader emits normalised
device coordinates directly, and

> **`v8` (TEXCOORD7) is the quad's centre in NDC.** `v6`/`v7` are its basis vectors, and the int16
> `POSITION` is only a local corner offset.

This confirms the shipped shader patch is correcting the right value, and finally explains why:
`v8` is the only thing that decides where the reticle lands.

### The input signature does not match how the draws are fed

Nine registers are declared: `v0` COLOR0, `v1` POSITION0 (int), `v2` TEXCOORD0 (int), and
`v3`-`v8` as TEXCOORD1-5,7. `v0`/`v1`/`v2` fit the 12-byte stream exactly. `v3`-`v8` cannot -
they need a second, per-instance stream.

**Indexed draws are where five-stream bindings live.** `DrawIndexedInstanced` (vtable slot 13)
had never been hooked; only `DrawInstanced` (slot 12) was. Hooking it shows draws with
`streams=5` - the five streams §5 originally claimed - so that claim was right, but about a draw
path we had never observed. It has its own sample budget, because sharing one let the couple of
thousand `DrawInstanced` calls drain it before any indexed draw was reached.

**But the reticle is not drawn there.** Across 12,000 sampled indexed draws, **zero** use the
reticle shader; they use `4ea98d8b` and `ee4689ce`.

### The open contradiction, sharpened

Every one of the 12 pipelines built from the reticle shader has an identical input layout, and it
is not consistent with the draws:

| Element | Slot | Offset | Format | Class |
| --- | --- | --- | --- | --- |
| POSITION[0] | 0 | 0 | `R32G32B32_FLOAT` | PER_VERTEX |
| COLOR[0] | 0 | 12 | `R8G8B8A8_UNORM` | PER_VERTEX |
| TEXCOORD[0] | 0 | 20 | `R32G32_FLOAT` | PER_VERTEX |
| TEXCOORD[1,2,3,4,5,7] | 0 | **0** | `R32G32_FLOAT` | PER_VERTEX |

Three problems at once:

1. **No `PER_INSTANCE` element exists anywhere**, so `v3`-`v8` have no per-instance source.
2. Six inputs alias onto **offset 0** of the same slot, which cannot feed six distinct values.
3. The layout implies a stride of at least 28 bytes, but the bound buffer has **stride 12**.
4. The layout declares POSITION as float3 while the shader reads it as int.

A layout this inconsistent cannot be what renders a working reticle - yet the reticle does render
at 100%. So the draws being attributed to this shader are not the draws that produce it. Pipeline
attribution is `thread_local` and looks sound, which points at the map being **incomplete** rather
than wrong.

**Resolved.** It was not a missing entry point - see below.

## 4f. The anchor, found and read (2026-08-30)

### There are two layouts, and the cap hid the real one

Logging was capped at 12 pipelines. All twelve fired during load and were identical, which made
it look like the shader had exactly one (impossible) layout. Deduplicating by layout signature
instead of capping revealed a **second** layout, created during gameplay:

| Semantic | Slot | Offset | Format |
| --- | --- | --- | --- |
| TEXCOORD[0] | 0 | 0 | `R16G16_SINT` |
| COLOR[0] | 0 | 4 | `R8G8B8A8_UNORM` |
| POSITION[0] | 0 | 8 | `R16G16_SINT` |
| TEXCOORD[1] .. [5], [7] | **1** | 0, 16, 32, 48, 64, **80** | `R32G32B32A32_FLOAT` |

Slot 0 is exactly the 12-byte stride we had been reading, and it **independently confirms the
byte decode in §4d**: texcoord at 0, colour at 4, position at 8.

Recording each pipeline's layout and reporting it per draw shows **~99% of sampled UI draws use
this layout**, so these are the real draws, not warm-up.

### The anchor is on slot 1, bound with stride 0

`v8` lives at slot 1, byte offset 80. Slot 1 *is* bound - always as part of a single
`IASetVertexBuffers(startSlot 0, numViews 5, ...)` call - but with a stride of **0, 4 or 16, never
96**.

**Stride 0 is the point.** A zero-stride vertex buffer makes every vertex read the same bytes,
which is the standard way to push per-object constants through the input assembler, and it is
exactly how the per-quad `v3..v8` are supplied. Every probe that filtered on
`StrideInBytes >= 96` excluded the only case that mattered and reported nothing - which is why
this looked for a long time like slot 1 was never bound at all.

Filtering on `SizeInBytes` instead reads it correctly, and the values come out as clean NDC:

```
ANCHOR AIM stride=0 v6=(0.0028,-0.0000,0.0000) v7=(-0.0000,-0.0050,0.0000)
                    v8=(0.1091,-0.6111,1.0000)  scale v3=(0.0625,0.0625)
```

`v8.z` is exactly `1.0`, matching the shader writing `o0.w = 1.0` with no perspective divide.
**The anchor is now directly readable at runtime**, which is what the whole hunt was for.

### What is not yet done

Reading at a fixed offset 80 is only correct for the draws whose slot-1 buffer starts at the
element base. Others come back shifted by whole float4s - `v6` reading as `(1,1,1)` and `v8`
holding what should be `v6` is the signature. So a wide capture mixes valid anchors with
misaligned ones, and diffing 100% against 200% currently produces noise rather than a clean
answer.

**Done.** The read now derives `slot`, and the offsets of `v6`/`v7`/`v8`, from each pipeline's own
input layout, recorded at creation for every pipeline carrying a `TEXCOORD7`. That removes the
shifted reads entirely - 1600 clean anchors per run.

### The UI has five coordinate bases, and all of them scale correctly

`v6.x` and `v7.y` are the basis vectors that turn the int16 POSITION into NDC, so `2/v6.x` is the
width of the space that position is expressed in. Decoding them across a matched pair:

| Implied basis | 100% | 200% | Reading |
| --- | --- | --- | --- |
| 1280x720 | 373 | 273 | virtual, resolution-independent |
| 1429x800 | 209 | 178 | virtual, resolution-independent |
| 720x400 | 188 | 140 | virtual, resolution-independent |
| 1920x1080 | 8 | 0 | raw buffer pixels |
| 3840x2160 | 24 | 5 | 2 units per buffer pixel |
| 7680x4320 | 0 | 12 | 2 units per buffer pixel |

At first glance the 1920x1080 family vanishing at 200% looks like the bug. **It is not.** At 100%
the buffer is 1920x1080, so its raw-pixel basis is 1920x1080 and its two-units-per-pixel basis is
3840x2160. At 200% the buffer is 3840x2160, so those become 3840x2160 and 7680x4320 - both
present. Every family has a correct counterpart at the other resolution.

So **every anchor basis scales correctly with the render buffer**, and the three virtual spaces
are resolution-independent by design. Nothing in the anchor data is malformed at 200%.

### Where that leaves it

The anchor is readable, correct, and scales properly - yet the reticle does not render at 200%.
The failure is therefore *not* a wrong scale factor in `v6`/`v7`, and not a malformed `v8`.

Remaining candidates, in the order worth testing:

1. **The reticle's `v8` is computed correctly but from a stale or wrong screen size** - would show
   as a valid-looking NDC value that simply points off-screen. Testable by finding the specific
   anchor that differs between the runs beyond sampling noise; the current diff is dominated by
   frame-to-frame variation, so it needs the same frame index in both runs, not just the same
   scene.
2. **The reticle draw is culled or scissored out** rather than mispositioned. The viewport and
   scissor logging already exists (`Log Viewports`) and has never been checked against a 200% run
   specifically for the UI pass.
3. **Something upstream stops emitting the quad at all** at 200%, in which case no anchor exists
   to find and the search should move to the CPU side that fills the slot-1 buffer.

Candidate 2 is cheapest and has not been tried.

### Colour did not discriminate

Vertex colour is captured now, but the byte order did not decode cleanly: the most common aiming
colour `0xFFB860FF` reads as pink under RGBA and purple under BGRA, and neither matches the
reticle's on-screen orange. Either the field is a modulation rather than a final colour, or the
format differs per draw. Not worth more guessing - the shader disassembly answers this too.

---

## 5. Eliminated hypotheses

Each was tested, not assumed.

| Hypothesis | How tested | Result |
| --- | --- | --- |
| A float→int16 store truncates the coordinate | Scanned `.text` for `cvttss2si` + 16-bit store + the `16.0f` constant; 15 sites; hardware execute breakpoints on all | All dead in gameplay or storing small values (`6654`, `15`) |
| A packed 32→16 store (`packssdw`, or wide store after a convert) | Broadened scan: 86 packs + 103 wide stores; software breakpoints on all 189 | 5 execute; **none truncates** — all handle 1/16 fixed point at 32-bit or float precision |
| Integer narrowing from the coordinate fields | Scan for 32-bit load from `[reg+0x1C]`/`[0x20]` then 16-bit store of that register | **Zero** instances (33 apparent hits were register reuse; four disassembled and confirmed coincidence) |
| 16-bit arithmetic wrapping in the computation | Scan for `shl reg16,4` / `imul reg16,…,16` feeding a store; 12 sites; trapped | Live ones carry `1920px`, `1104px`, `1100px` — **window space**, inside int16. This is why stock 4K works |
| A 16-bit read of a 32-bit coordinate field | 288 reads of `[reg+0x1C]`/`[0x20]`; trapped all, printed the full 32 bits at each | Nothing holds a wrappable coordinate; apparent hits are adjacent int16 fields or `0xFFFF` sentinels |
| The value is rewritten and can be watched | Hardware read+write watch on the wrapped value in memory | **Zero** hits across aiming, lowering, weapon switches, reloads — while a self-test watch fired normally |
| `PAGE_GUARD` on upload buffers catches the write | Guarded 40 buffers at creation | `VirtualProtect` succeeds but **no fault is ever delivered** — driver-mapped memory does not honour it |
| Guarding all heaps to catch the one-time write | Tried | **Froze the game three times.** Thread stacks must be excluded, and even then exception dispatch itself faults |
| Setting `bufferSizeX` in `mgs4.ecf` avoids the wrap | Config edit, with and without `dynamicResolution` | The engine overwrites `bufferSize` with `windowSize` during init regardless |
| The coordinate travels through `CopyBufferRegion` as an exact centre-screen pair | Searched every copy source for `(bufferX/2*16, bufferY/2*16)` | Never found — the reticle is not exactly centre-screen, so the value-based search was wrong in principle |

---

## 6. Tooling (all behind undocumented settings keys)

All are read with `getValueOptional`, absent from the Config Tool, and **stripped if the Config
Tool is opened** — edit `MGSPatriotFix.settings` by hand.

| Key | What it does |
| --- | --- |
| `Disassemble RVA` / `Disassemble Bytes` | Disassembles comma-separated hex RVAs at startup, via Zydis, in-process. `.text` is Steam-encrypted on disk, so this is the only way to read real code |
| `Find String` | Finds an ASCII string in the image and lists every instruction referencing it. Comma-separated; **always include a control such as `render.bufferSizeX`** |
| `Find Displacement` | Every instruction addressing a given struct offset. Found the draw-list container's users |
| `Dump Floats` | Logs the value at hex RVAs as float and int32 |
| `Scan Fixed Point Sites` | Six-pass `.text` scan for candidate conversions (see §5). Populates the candidate list |
| `Trap Fixed Point Sites` | Software `INT3` breakpoints on every candidate. F9 arms, F10 disarms and reports which executed |
| `Breakpoint RVAs` | Hardware execute breakpoints on up to four hex RVAs |
| `Watch Wrapped Value` / `Guard Wrapped Region` / `Guard Record Heap Writes` | Hardware watches and `PAGE_GUARD` variants — all superseded, kept for the record |
| `Watch Render Size Reads` / `Watch Cached Render Size` | Watches the render size globals, or float copies of them |
| `Dump Wrapped Context` | Finds a wrapped coordinate and dumps 256 bytes around it as hex, int16 and int32 |
| `Guard Upload Buffers` | Guards each upload buffer at creation. **Does not work** — see §5 |
| `Watch Staging Writes` | Current tool. Traces UI quad vertices back through the copy into CPU memory, prints them, and arms a write watch. Sampling is tied to the **aim button** |
| `Log Viewports` | UI draw logging: shader checksum, per-stream buffer/stride/size and contents, call stack, `CopyBufferRegion` calls |
| `Log Render Target Allocations` | Render size monitor — logs `bufferSize`/`windowSize` whenever they change |

### D3D12 hooks installed

Device vtable: `CreateSampler` (22), `CreateCommittedResource` (27), `CreatePlacedResource` (29),
`CreateGraphicsPipelineState` (10), `CreateCommandList` (12), `CreateCommittedResource1` (53),
`CreateCommittedResource2` (68).
Command list vtable: `DrawInstanced` (12), `CopyBufferRegion` (15), `RSSetViewports` (21),
`RSSetScissorRects` (22), `SetPipelineState` (25), `SetGraphicsRootConstantBufferView` (38),
`IASetVertexBuffers` (44).

---

## 7. Methodology notes — read before adding another probe

Most of the wasted effort in this investigation came from a single recurring mistake: **treating a
probe's silence as evidence, without first proving the probe could speak.**

Concrete instances:

- The upload-heap filter (`Type != HEAP_TYPE_UPLOAD`) rejected **every** buffer this engine
  allocates. Five probes were built on top of that bookkeeping. "Not in a mapped buffer" was read
  as "the CPU never writes here" for hours.
- `PAGE_GUARD` on upload buffers reported 40 guarded and 0 writes for two runs. A read counter,
  added late, showed **no faults at all** — the guards never worked.
- A watch self-test was written by the one thread that arming deliberately skips, so it could
  never fire.
- A hardware watch was armed with no exception handler registered.
- A `Rip - 1` assumption made every software breakpoint lookup miss by one byte; `ExceptionAddress`
  is the correct source.
- Frame interpolation (NVIDIA Smooth Motion) makes PIX captures come back with no draw calls.
  Turn it off for diagnostics.

**Rules that follow:**

1. Every probe reports how many times it *looked*, not only what it found.
2. Every scan includes a control that must match (`render.bufferSizeX`, `+4E160B`, a self-written
   variable).
3. Check the log's `Fix built:` line matches the build just deployed. Sessions end with
   `Session ended cleanly`.
4. Anchor searches on something observed, not on a guessed value. Every value-based search in this
   investigation failed; every structural one (call stack, struct offset, copy tracing) worked.

---

## 8. Environment

- Game: `F:\SteamLibrary\steamapps\common\METAL GEAR SOLID 4`
- Build: `MSBuild` at
  `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe`;
  needs `C:\Program Files\CMake\bin` on `PATH` for the Config Tool
- Deploy: copy `x64\Release\MGSPatriotFix.asi` to `<game>\MGS4\`
- Log: `<game>\logs\MGSPatriotFix_Game.log`, recreated per launch
- **Launcher bypass installed**: `Launcher\launcher.exe` is a wrapper that goes straight into the
  game; the original is `launcher_original.exe`. Turnaround is seconds
- Test config for this hunt: `Fix UI At High Resolutions=0`, `Internal Resolution Scale (%)=200`,
  `Max Internal Buffer Width=8192` — a 7680×4320 buffer with the crosshair visibly broken
- Ghidra is installed but its script compiler is broken in this environment; the in-process
  disassembler replaces it
