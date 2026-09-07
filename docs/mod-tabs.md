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
| `Ini` | yes | The file the tab edits. A bare file name; it must sit beside the manifest. If it does not exist yet it is created on the first save. |
| `Asi` | no | Your `.asi`. The tool uses it to tell the user when the files sit somewhere the game does not load from. |
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

## What the tool does with it

- The tab lists only the blocks you wrote. **Nothing else in your file is ever written**: a save
  replaces the value part of each changed line and leaves comments, order and unknown keys as
  they were. A key that has no line yet is added at the end of its section.
- A mistake in the manifest (a missing `Type`, an `int` without `Min`/`Max`, a `Default` that is
  not one of the `Choices`) does not hide the tab. The tab shows the problem in orange and
  leaves that row out, so you see it the first time you try it.
- Values in the file that the tool cannot read (text in an `int`, a choice not in the list) show
  as the `Default`; the file keeps its value until the user changes that row.
- If the tool's built-in table already knows your `.ini`, the manifest wins.
- The tool finds the manifest in the install folder's own `scripts\` and friends too, where a
  zip extracted one level too high lands, and tells the user the mod is not running from there.

## Trying it

Run the tool from the game folder; the tab appears between Graphics and Troubleshooting. To
open straight onto it: `"MGS4 SSAA and Aspect Ratio Enabler.exe" --tab MyMod`.
