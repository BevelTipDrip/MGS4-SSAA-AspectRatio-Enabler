# Feature and bug index

One document per feature. Every bug belongs to the feature whose patch causes or exposes it, so
a bug introduced by the supersampling patch is filed under Supersampling regardless of what it
looks like on screen.

| Feature | Setting |
| --- | --- |
| [Supersampling](supersampling.md) | Internal Resolution Scale (%) |
| [Shadows](shadows.md) | Shadow Resolution Scale (%), Shadow Softness (Samples) |
| [Anti-aliasing](anti-aliasing.md) | FXAA, FXAA Quality |
| [Texture filtering](texture-filtering.md) | Anisotropic Filtering |
| [Window resolution](window-resolution.md) | Window Aspect Ratio, Window Resolution |
| Aspect ratio (ultrawide, 4:3) | Fix UI Aspect, Aspect Pool Spread — the implementation and its notes are a private module at external/ultrawide; the fork builds without it |
| [Tooling](tooling.md) | not user-facing |

**No counts here on purpose.** This table carried Active / Fixed / Unverified totals for about an
hour before they were wrong - a bug moved and a new entry was added, and the index said neither.
A summary that has to be updated by hand is a second place for the truth to live, and the one
that goes stale silently. Open the document.

---

## Why every entry carries an evidence level

A claim with nothing behind it once sat in these notes beside claims backed by GPU captures, and
nothing marked them apart. It was repeated as fact for weeks, and eventually "fixed" — a fix
asserted for a bug never confirmed to exist. The level makes that impossible to hide.

| Level | Means | Test to apply |
| --- | --- | --- |
| **Measured** | A number, capture or log supports it | Can you point at the artefact? |
| **Observed** | Seen on screen and reproducible on demand | Can you make it happen again right now? |
| **Reported** | Someone described it; we have not reproduced it | Who said so, and when? |
| **Inferred** | Deduced from other facts, never directly seen | Which facts, and does the chain hold? |
| **Retracted** | Was asserted, then found to have no support | What was the original claim, and why did it fail? |

**Inferred is not evidence.** It is a hypothesis with a note attached. Anything at Inferred or
Reported needs promoting to Observed or Measured before it justifies a code change — and if it
cannot be promoted, that is itself the finding.

Retracted entries are never deleted. A claim that turned out to be wrong is worth more as a
record than as a gap, because the next person will otherwise re-derive it.

---

## Bug entry format

```markdown
### SS-001 — one-line summary

- **Status:** Active | Fixed in <version> | Retracted
- **Evidence:** Measured | Observed | Reported | Inferred | Retracted
- **Affects:** which settings and values trigger it
- **Repro:** the shortest reliable way to see it
- **Evidence detail:** the actual artefact — a number, a log line, a capture
- **Screenshots:** files under `evidence/`, or "none"
- **Popups:** any dialog the bug produces, quoted exactly, or "none"
- **Cause:** the mechanism, if known. "Unknown" is a valid and useful answer
- **Fix:** what changed and where, once fixed
- **Verified:** how the fix was confirmed, and by whom
```

### Screenshots

Live in `evidence/`, named `<ID>-<what-it-shows>-<resolution>.jpg`. Downscaled to about 1100px
and JPEG-encoded — these are records of whether an element is on screen, not texture references,
and a repository is a bad place for 2MB PNGs.

**Capture a pair wherever the bug is conditional.** A screenshot of something missing proves very
little on its own; the same scene at a working setting beside it proves a great deal. `shot.ps1`
captures the game's client area, so shots stay comparable across window sizes.

### Popups

Record every dialog verbatim, including ones from Steam, the launcher or Windows rather than the
game. Quote the title and body exactly — the wording is what a search matches, and a paraphrase
is useless to anyone hitting it later. Note which process owns it, because that determines
whether it can be dismissed programmatically: a dialog drawn inside another application's window
cannot be found by enumerating top-level windows.

IDs are per feature (`SS-` supersampling, `SH-` shadows, `AA-` anti-aliasing, `TF-` texture
filtering, `WR-` window resolution, `AR-` aspect ratio, `TL-` tooling) and are **never reused**,
including for retracted entries.

---

## Conventions

- **Quote observations verbatim.** Paraphrasing is how "the reticle vanishes" became "and some
  menu text breaks". If the user described it, use their words and say so.
- **Say who observed it.** "Reported by the user, 2026-08-30" is checkable; "it is known that"
  is not.
- **Record the resolution and settings.** Nearly every bug here is conditional on a render size,
  and "at high resolution" is not a render size.
- **A fix is not fixed until verified.** Note what confirmed it — a log line, a screenshot, a
  play session — and prefer real play over synthetic checks.
- **When a bug turns out not to exist, retract it in place.** Change the status, keep the entry,
  and write down what the original claim was and why it failed.

---

## Adding a feature

Copy an existing document. Each one has:

1. **What it does** — one paragraph, user-facing.
2. **What it patches** — the game code and values, with RVAs and the source file that implements
   it. This is the part that makes a bug diagnosable rather than mysterious.
3. **Limits** — clamps and ceilings, each with the reason it exists. A limit whose reason has
   expired is a bug in itself.
4. **Active bugs** — checklist, newest first.
5. **Fixed** — with the version and what verified it.
6. **Retracted / unverified** — claims that did not survive scrutiny.
