# A tab for your mod's config file

If your MGS4 mod keeps its settings in a small `.ini` and has no settings tool of its own, the
MGS4 SSAA and Aspect Ratio Enabler tool can edit it on a tab of its own. You describe the
settings in a **manifest**, a second small INI file shipped next to your `.ini`, and the tab
appears whenever the tool finds it. Nothing on this side needs to change and no pull request is
needed.

## The manifest

Name it `<anything>.MGS4Enabler.ini` and put it in the folder your `.ini` lives in (the same
folders the game's ASI loader scans: `MGS4\`, `MGS4\scripts\`, `MGS4\plugins\`, `MGS4\update\`).
A copy of `docs/mod-tabs/Example.MGS4Enabler.ini` is a good starting point.

```ini
[Mod]
Name = MyMod
Ini = MyMod.ini
Asi = MyMod.asi
Url = https://github.com/someone/MyMod
Blurb = One or two sentences shown at the top of the tab.\nA "\n" starts a new line.

[Settings/TargetFrameRate]
Label = Target frame rate
Type = int
Default = 60
Min = 15
Max = 1000
Help = What the setting does, in your words. Shown as the tooltip.

[Graphics/Mode]
Label = Render mode
Type = choice
Choices = Fast|Balanced|Quality
Default = Balanced

[Graphics/VSync]
Label = V-Sync
Type = bool
Default = true
BoolText = true|false
```

### `[Mod]`

| Key | Required | Meaning |
| --- | --- | --- |
| `Name` | yes | The tab's title. |
| `Ini` | yes | The file the tab edits. A bare file name, looked for beside the manifest and then in the game's install folder (where some mods keep their settings, beside the launcher). If it exists in neither it is created beside the manifest on the first save. |
| `Asi` | no | Your `.asi`. The tool looks for it in the folders the game's loader scans and tells the user when it is only found somewhere the game does not load from. Leave it out and no such check is made. |
| `Url` | no | A link shown on the tab. |
| `Blurb` | no | Text above the settings. `\n` breaks a line. |

### One block per setting

The block's name is `[Section/Key]`: the section header and key name in **your** `.ini`. For a
file with no section headers at all, write `[Key]`.

| Key | Applies to | Meaning |
| --- | --- | --- |
| `Type` | all, required | `int`, `bool` or `choice`. |
| `Label` | all | The row's name on the tab. The key name when absent. |
| `Help` | all | The tooltip. `\n` breaks a line. |
| `Default` | all | What the row shows when the key is missing from the file. Nothing is written until the user saves. |
| `Min`, `Max` | `int`, required | The spinner's range. |
| `Choices` | `choice`, required | The values exactly as they are written in your file, with `\|` between them. |
| `BoolText` | `bool` | What true and false are written as, true first: `true\|false`, `on\|off`. `1\|0` when absent. |

## Features: saying what a setting switches on

Several MGS4 mods patch the same few things - shadow resolution, dynamic resolution,
anisotropic filtering - each behind a value in its own config file. Two of them on at once
stack or fight at start-up, in whatever order the loader ran them. To let the tool keep exactly
one mod on each thing, a block can say which **feature** its key switches on:

```ini
[ShadowResolutionOverride]
Type = int
Min = 0
Max = 8192
Default = 0
Feature = shadow-resolution
Off = 0

[Enhancements/Anisotropic Filtering Level]
Type = int
Min = 1
Max = 16
Feature = anisotropic-filtering
AlwaysOn = true
```

| Key | Meaning |
| --- | --- |
| `Feature` | The shared name of the thing this key switches on. Use one from the list below, or a new one; ids are compared as text. |
| `Off` | The value at which your mod does nothing for that feature. Required, except for a `bool` (its false spelling) or with `AlwaysOn`. |
| `AlwaysOn` | `true` when your mod patches the feature whenever it is installed, whatever the value. |

Known ids: `shadow-resolution`, `shadow-filter`, `dynamic-resolution`, `anisotropic-filtering`,
`internal-resolution`, `frame-rate`, `skip-intro`, `motion-blur`, `api`, `window-mode`.

With that declared:

- On every tab, a row whose feature is also on in another mod says so beside its label, in
  orange, naming the mod and its value.
- On save, for each feature that is on in more than one mod, the tool asks which one keeps it
  and sets the others to their `Off` values. An `AlwaysOn` claim always wins; the tool says so
  and offers to turn the others off.
- The Enabler's own `.asi` reads the manifests at start and stands down on any feature another
  installed mod has on, so its patch never stacks on yours. It also logs a warning naming any
  two other mods that both have a feature on.

## What the tool does with it

- The tab lists only the blocks you wrote. **Nothing else in your file is ever written**: a save
  replaces the value part of each changed line and leaves comments, order and unknown keys as
  they were. A value your file had in quotes is written back in quotes. A key that has no line
  yet is added at the end of its section.
- A mistake in the manifest (a missing `Type`, an `int` without `Min`/`Max`, a `Default` that is
  not one of the `Choices`) does not hide the tab. The tab shows the problem in orange and
  leaves that row out, so you see it the first time you try it.
- Values in the file that the tool cannot read (text in an `int`, a choice not in the list) show
  as the `Default`; the file keeps its value until the user changes that row.
- The tab exists only while the mod does: your `.ini` is found, or the `.asi` you named is. A
  manifest on its own (say, one shipped ahead of the mod) shows nothing.
- The tool finds the manifest in the install folder's own `scripts\` and friends too, where a
  zip extracted one level too high lands, and tells the user the mod is not running from there.

## Trying it

Run the tool from the game folder; the tab appears between Graphics and Troubleshooting. To
open straight onto it: `"MGS4 SSAA and Aspect Ratio Enabler.exe" --tab MyMod`.
