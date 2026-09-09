# Feature ownership: one mod per patched thing

**Settings:** none of its own — it acts on every setting that patches something another mod
might also patch
**Implements:** `shared/features.hpp`, `shared/manifest_claims.hpp`, `shared/compat_table.hpp`,
`asi/src/core/compat.cpp`, `tool/src/compat.cpp`, the ownership section of `tool/src/ui.cpp`

## The problem

Several MGS4 mods change the same few things: shadow resolution, dynamic resolution,
anisotropic filtering, the frame rate. They load into one process, each installs its hooks
synchronously as the loader reaches it, and none of them knows the others exist. Two mods that
patch the same thing therefore stack (two multipliers applied to one value), fight (one
detour's write lands after the other's and wins), or produce whichever result the alphabetical
load order happens to give. The symptom is a setting that silently does nothing, or a value
nobody chose.

Nothing here can stop two other mods from doing that to each other. What it can do is keep
this mod out of it, tell the user when it is happening, and offer to fix it in one place.

## The idea: a feature id

A **feature** is the shared name for a thing a mod may patch: `shadow-resolution`,
`shadow-filter`, `dynamic-resolution`, `anisotropic-filtering`, `internal-resolution`,
`frame-rate`, `skip-intro`, `motion-blur`, `api`, `window-mode`. The ids are free-form strings
compared exactly; `shared/features.hpp` only supplies display names for the known ones, so a
mod may invent an id and everything still works — two mods agreeing on a new string is enough.

Every setting that patches a feature declares three things: which feature, and what value
means *off*, and whether it is on regardless of value.

- **Ours** are in the `kOurs` table in `shared/features.hpp`: section, key, feature, off value.
  For example Shadow Resolution Scale claims `shadow-resolution` and is off at `100`.
- **Another mod's** are in its manifest (`<Mod>.MGS4Enabler.ini`, see
  [mod-tabs.md](../mod-tabs.md)): a `[Section/Key]` block with `Feature = <id>` plus either
  `Off = <value>` or `AlwaysOn = true`. A `bool` key needs neither; its false spelling is the
  off value.

`AlwaysOn` is the case where a mod patches whenever it is loaded, with no setting to turn it
off — MGSPatriotFix's anisotropic filtering is one. Such a claim cannot be conceded, so it
always wins.

## Deciding whether a claim is on

`mgs4e::manifests::Scan` (`shared/manifest_claims.hpp`) does the whole job, and both binaries
call the same code so they cannot disagree:

1. Find every `*.MGS4Enabler.ini` in the folders the loader reads (`MGS4`, `MGS4\scripts`,
   `MGS4\plugins`, `MGS4\update`) and the same names one level up, which is where a zip
   extracted too high puts them. One entry per mod ini, a loadable copy preferred.
2. For each, find the mod's own ini (beside the manifest, else in the install folder) and its
   `.asi`. A manifest that names no `.asi` is judged installed by its ini existing; a manifest
   found only in a non-loadable folder does not count as installed.
3. Read the current value of every claimed key out of that mod's ini.
4. A claim is **active** when the mod is installed **and** (`AlwaysOn`, or the value is not the
   off value).

"Not the off value" is `features::IsOff`, which compares text, then numbers (`0`, `0.0` and
`0.000` agree; `1.0` and `1` agree), then bool spellings (`true`/`yes`/`on`/`1`). An absent key
counts as off, since that is every mod's default. Values keep their quotes on the way back out.

## What the .asi does

At startup, after its own settings are read, `compat::Detect()` scans the manifests and looks
for the mods in the hardcoded table. Then, at each patch site, the feature's owner decides
whether the patch is installed at all:

```cpp
if (fShadowResolutionScale > 1.0
    && !mgs4e::compat::Yields(mgs4e::keys::Graphics, mgs4e::keys::ShadowResolutionScale))
```

`Yields` says true when either source says another mod owns the feature:

- the hardcoded table in `shared/compat_table.hpp`, for a mod with no manifest (currently
  MGSPatriotFix): a rule per colliding key, `WhenLoaded` or `WhenNonZero`, checked against
  that mod's own settings file, which is only ever read;
- an active manifest claim on the same feature as our setting.

Either way the hook is never installed, and the log says which mod and which of its keys
decided it. Standing down at install time rather than at run time is deliberate: nothing is
patched, so there is nothing to unwind, and the log line is written once.

`compat::LogOverlaps()` then reports the case this mod cannot fix — two *other* mods with the
same feature on. Their patches will stack whatever we do, so it warns, names both mods and
their values, and points at the tool:

```
Mod overlap: Shadow resolution is turned on in both MGS4.FusionFix (ShadowResolutionOverride
= 4096) and MGSPatriotFix (Custom Shadow Resolution = 4096); their patches will stack at
start-up. Open the ... tool and choose one - it turns the others off.
```

## What the tool does

The tool sees the same claims plus its own fields, so it can act instead of only reporting.
`ActiveOwners()` builds one `Owner` per claim that is currently on — from our rows and from
every mod tab — and everything else works from that list.

**While editing.** `RefreshOverlapNotes()` appends a note to the label of any row whose feature
is on somewhere else: `- also on in MGS4.FusionFix (4096)`, in orange on a mod tab. It runs on
load and after every save, so the note follows what is actually in the files. A field a mod
owns outright (an `AlwaysOn` claim, or a hardcoded-table rule in force) is greyed out instead,
with the owning mod and value shown beside it; there is no choice to offer there.

**On save.** `ResolveOverlaps()` runs before anything is written. For each feature with two or
more owners on:

- if one of them is `AlwaysOn`, it wins: a yes/no dialog names it, lists the others and offers
  to turn them off;
- otherwise a single-choice dialog lists every owner as `mod: label = value` and asks which
  keeps the feature.

Whatever is not kept is set to its declared off value, in its own file, and the save proceeds.
Cancelling the dialog cancels the save, so the user is never left with a file half-resolved.
Only keys a manifest lists are ever written to another mod's ini; everything else in that file,
comments included, is carried through untouched.

## Boundaries

- A claim describes a *file*, not a running patch. If a mod ignores its own config, or patches
  something its manifest does not mention, nothing here can tell.
- Our stand-down is by feature id. Two mods that patch the same thing under different ids look
  independent, which is why the id list is worth keeping short and shared.
- The hardcoded table exists for mods that ship no manifest. Anything with a manifest should
  use the manifest; the table is not the place to grow.
- The tool resolves at save time only. Editing another mod's ini by hand between saves is
  invisible until the next load.

---

## Active bugs

None recorded.

---

## Fixed

None recorded.
