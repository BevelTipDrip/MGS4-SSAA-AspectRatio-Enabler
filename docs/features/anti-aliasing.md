# Anti-aliasing

**Settings:** `FXAA`, `FXAA Quality`
**Implements:** `asi/src/features/graphics_settings.cpp`, `ApplyShadowAndAntiAliasing`

## What it does

Forces the game's FXAA on or off, and sets its quality level. The game exposes the toggle in its
own display menu but not the quality, and either needs a restart to take effect.

Worth keeping on even with supersampling: extra resolution fixes geometry edges but not shader
aliasing or specular sparkle.

## What it patches

**FXAA** is read through a getter that builds its key from `render.fxaa` plus a platform suffix
(`fxaa_PS5`, `fxaa_Switch_Docked` and so on), so there is no single global to write. The getter
is specific to this setting though, and by the end of it the answer is in EAX:

```
MOVZX EAX,BL           ; the parsed value
MOV   RBX,[RSP+0x60]   ; <- hooked here, EAX already set
...
RET
```

**FXAA Quality** is parsed as a double, narrowed to float and stored:

```
CVTSD2SS XMM1,XMM0
MOVSS    [global],XMM1   ; <- hooked here, so XMM1 holds the value about to be stored
```

| Hook | Matched at |
| --- | --- |
| FXAA | `+0x65CC49` |
| FXAA Quality | `+0x663DA6` |

The engine's own config documents the quality as Slow(0), Medium(1), Fast(2) and ships 1 — so
**0 is the best-looking setting, not the worst**, and the naming is about cost rather than
result.

## Limits

None.

---

## Active bugs

None recorded.

---

## Fixed

### AA-001 — our FXAA override does win over the game's own setting

- **Status:** Confirmed, 2026-08-31
- **Evidence:** Observed — engine log plus visual comparison at native resolution
- **Affects:** any case where the in-game display menu and this setting disagree

Tested with three cold boots of the same stage (`s01a10l_1`) at 100% internal scale, driven by
`boot_to_aim.ps1`. The game's own setting was on throughout; only ours changed.

| Run | Our setting | What the hook logged |
| --- | --- | --- |
| on-1 | `FXAA=1` | `MGS4: FXAA: Hook installed.` |
| on-2 | `FXAA=1` | `MGS4: FXAA: Hook installed.` |
| off | `FXAA=0` | `MGS4: FXAA: on -> off.` |

`on -> off` is the value the **engine itself** resolved from its own setting, being read as on and
replaced with off. The two matching runs log no change line at all, because wanted equalled
current — which is the correct behaviour and also confirms the message is not printed
unconditionally.

The rendered result was then confirmed by eye at native 4K, where the difference between the two
is clear. Matching crops, 2x nearest-neighbour so no resampling softens the edges:
[on](evidence/AA-001-fxaa-on.png) / [off](evidence/AA-001-fxaa-off.png).

These are **PNG, not JPEG like the other evidence here, and that is deliberate** — JPEG destroys
exactly the high-contrast edge detail the comparison rests on, which would make the two images
look more alike than they are.

## Limits of the automated check

**Edge-energy measurement could not confirm this on its own, and should not be trusted as an
automated AA gate.** Both the mean gradient and the percentile version were run over the same
captures:

| Region | metric | on-1 | on-2 | off | spread across the two "on" runs |
| --- | --- | --- | --- | --- | --- |
| left | mean | 11.0084 | 11.1942 | 11.3788 | 0.186 |
| left | p99 | 97.11 | 97.56 | **101.01** | 0.45 |
| left | p999 | 164.81 | 168.24 | 166.35 | 3.43 |
| right | p99 | 21.54 | 21.96 | 21.73 | 0.42 |

Only left `p99` separates. `p999` and the entire right-hand region place the "off" run *between*
the two "on" runs, which a real effect would not do. Averaging is the core problem — aliasing
lives on the small minority of pixels that sit on high-contrast edges, and most of a frame is
smooth shading that cannot change — but restricting to the steepest percentiles was not enough to
fix it either.

Two runs give a gap, not a standard deviation. Any future automated AA check needs several runs
per condition and a scene chosen for hard edges, and even then the human comparison at native
resolution is what actually settled this one. The downscaled, compressed copies used for review
make the difference much harder to see than the original captures do.

---

## Retracted / unverified

None recorded.
