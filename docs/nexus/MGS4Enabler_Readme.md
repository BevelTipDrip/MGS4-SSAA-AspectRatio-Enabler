# MGS4 SSAA and Aspect Ratio Enabler

A graphics mod for METAL GEAR SOLID 4 in the Master Collection on PC. It adds supersampling,
higher resolution shadows, anisotropic filtering and FXAA control, lets you pick the window
resolution and aspect ratio, and fixes the HUD, menus, codec, videos and cutscenes so they look
right on ultrawide (21:9, 32:9), 16:10 and 4:3 displays instead of being stretched to fit.

It comes as an .asi that patches the game in memory as it starts, plus a small settings tool.
No game files are modified.

Source, screenshots and the issue tracker:
https://github.com/BevelTipDrip/MGS4-SSAA-AspectRatio-Enabler


## Known issues

Things that still take the window's shape on a 21:9 or 4:3 display. All of them are on the
list; none of them affect 16:9.

1. The in-game options menu, the one with the Nomad interior behind it, has the same scaling
   problem the codec calls had and needs the same kind of fix.
2. The 3D renders of items, weapons and the camo screen are not corrected yet.
3. The map render is not corrected yet.
4. Subtitles, in gameplay and in cutscenes, are stretched.


## What it fixes on ultrawide and 4:3

The game draws its entire interface in a 16:9 space and stretches that to whatever the window
is. On a 21:9 monitor everything comes out wide and the compass turns into an oval; on 4:3 it
all gets squashed. With the Expanded HUD setting, every element keeps its proper shape and the
corner widgets sit at the edges of the actual screen, where they belong.

Menus, the codec, loading screens and cutscenes are shown as the 16:9 picture they were
designed as, centred, with black bars over the rest of the window rather than the world leaking
past their edges. On an ultrawide the cutscene camera is zoomed back out so the full frame fits,
because the engine otherwise keeps the 16:9 width and cuts the top and bottom off.

Codec calls: the caller's portrait is rendered by its own camera, and that camera took the
window's shape, a narrow face on an ultrawide, a wide one on 4:3. It now keeps the 16:9 framing.

Pre-rendered videos (the opening's TV commercials and the like) were drawn over the whole window,
so on anything but 16:9 they were stretched. They now play pillarboxed on an ultrawide and
letterboxed on 4:3.

The difficulty select, the load list and the options screen were stretched across the full
window while the main menu next to them was not. They now follow the same 16:9 layout as
everything else.

Under the surface, the renderer was also drawing every non-16:9 picture into a centred 16:9 band
of the window, on both DirectX 11 and DirectX 12, which squeezed a 4:3 or 21:9 picture and left
garbage in the corners. That pass now covers the full window. Screenshots of all of this are on
the GitHub page.


## Features

Aspect ratio and HUD
- Window resolution and aspect ratio selection (16:9, 16:10, 21:9, 32:9, 4:3) independent of the
  game's own launcher settings.
- Three HUD modes: Stretched (the game's behaviour), Centered (every widget at its proper shape,
  interface centred in a 16:9 area) and Expanded (same, with the life bar, camo meter, weapon
  and item panels moved out to the real screen edges; wheels and menus stay centred).
- World-pinned labels (pickup names, Solid Eye stat blocks, targeting icons) stay on the
  objects they mark.
- 16:9 masking for menus, codec, loading screens and cutscenes, and a cutscene FOV fix for
  wide windows. Each is its own switch.
- Gameplay FOV adjustment, 50 to 200 percent. Cutscenes are not affected.

Image quality
- Internal resolution scaling up to 400 percent (supersampling). The window stays at your
  chosen resolution; the game renders larger and scales down.
- Shadow map resolution scaling and shadow filter sample count.
- Anisotropic filtering up to 16x.
- FXAA on/off and its quality level, which the game never exposes.

Housekeeping
- A settings tool that writes one file, MGS4Enabler.settings, with the defaults filled in.
  The .asi reads that file itself, so the tool is optional once the file exists; you can edit
  the file by hand and delete the exe if you prefer.
- Plays nicely with other mods: where another mod it knows about controls the same thing,
  that mod's setting wins and the tool tells you so.
- Nothing phones home. No updater, no telemetry.

16:10
- 16:10 displays get the same treatment as 4:3, with thinner bars: the HUD keeps its shape
  and the corner widgets go to the real edges, menus and cutscenes sit in the 16:9 area. Pick
  16:10 as the aspect ratio and a resolution from its list.


## Installation

Extract the zip into the game's install folder, the one that contains the MGS4 folder
(Steam: right-click the game, Manage, Browse local files). You end up with:

    METAL GEAR SOLID 4\
        MGS4Enabler.exe
        README.md
        UltimateASILoader_LICENSE.md
        MGS4\
            winmm.dll                  (Ultimate ASI Loader)
            scripts\MGS4Enabler.asi

Run MGS4Enabler.exe from that folder, pick your settings and save. That creates
MGS4Enabler.settings next to it, which the .asi reads every time the game starts. Changes take
effect on the next launch.

If another mod already gave you an MGS4\winmm.dll, it is the same loader; keep whichever is
newer. The loader runs every .asi in MGS4\scripts.

Do not put an .asi in the game's install folder itself. It is not loaded from there, and some
mods' config tools hang if they find one.

Works with the game set to either DirectX 11 or DirectX 12 in its options. The game ships set
to DirectX 11.

Windowed mode stutters compared with fullscreen, with or without this mod: the game presents
through the desktop compositor there. Turning the game's own V-Sync off helps a lot; fullscreen
is the smooth option.


## Settings

Everything is on the Graphics tab. Hover a setting in the tool for a longer explanation.

- Window Aspect Ratio (default: Use Game Setting). Overrides the output resolution. Choosing
  16:9, 16:10, 21:9, 32:9 or 4:3 reveals a list of resolutions for that shape. Leave it on Use Game
  Setting to keep whatever the game's launcher selected.
- Ultrawide HUD (default: Expanded HUD). How the HUD is laid out on a window that is not 16:9.
  Stretched HUD is the game's own behaviour. Centered HUD gives every widget its proper shape
  and centres the interface in a 16:9 area. Expanded HUD does that and moves the life bar, camo
  meter, weapon and item panels out to the real edges. Works from the actual window size, so it
  also applies on a native ultrawide or 4:3 display with the aspect left on Use Game Setting.
  No effect at 16:9.
- Solid Eye Overlay Fix (default: on). With a Centered or Expanded HUD, keeps the labels that
  follow things in the world on the objects they mark instead of sliding toward the centre with
  the rest of the interface.
- Menu Masking (default: on). Keeps the title screen, main menu, pause menu, codec and loading
  screens at 16:9 and blacks out the rest of the window. Needs a Centered or Expanded HUD.
- Cutscene Masking (default: on). Shows cutscenes as the 16:9 picture centred in the window
  with black bars over the rest. Pair it with Cutscene FOV Compensation on an ultrawide.
- Cutscene FOV Compensation (default: on). On a window wider than 16:9 the cutscene camera
  keeps the 16:9 width and crops the top and bottom. This zooms the camera back out so the full
  frame fits.
- FOV Adjustment (%) (default: 100). How much of the world the gameplay camera shows, relative
  to stock. On a 21:9 window, 133 restores the vertical view you would have at 16:9.
- Internal Resolution Scale (%) (default: 100). Renders internally above your display
  resolution and scales down. Up to 400, with the internal buffer capped at 8192 wide: roughly
  400 at 1080p, 200 at 4K.
- Shadow Resolution Scale (%) (default: 100). Raises shadow map resolution beyond the game's
  highest Shadow Quality. 200 doubles it in each dimension and costs four times the shadow
  memory.
- Anisotropic Filtering (default: 0, off). Maximum anisotropy for textures seen at a steep
  angle. The game's highest texture setting asks for 8x; 16x is the hardware maximum and
  nearly free.
- FXAA (default: on). Post-process anti-aliasing. Overrides the game's own toggle. Needs a
  restart either way.
- FXAA Quality (default: Slow). Slow, Medium or Fast, where slower looks better. The game ships
  Medium. Worth keeping on even with supersampling, since extra resolution does not fix shader
  aliasing.

Notes
- Internal Resolution Scale is expensive. Every 41 percent adds another whole screen's worth
  of pixels. At a 4K window 200 gives 7680x4320 and anything above that is clamped, which the
  log will tell you.
- Shadow scaling is applied on top of the game's own Shadow Quality option, so leave that at
  its highest.
- A window size that is not your desktop size is scaled to fill the display, keeping its
  shape: 4:3 on a widescreen monitor is pillarboxed, 21:9 on a 16:9 monitor is letterboxed. The
  game on its own paints such a picture unscaled in the top-left corner.
- The masking options and FOV Adjustment do nothing at 16:9. They default on because that is
  the framing the game was made for. Turn Cutscene Masking off if you would rather have
  cutscenes fill an ultrawide.
- The aiming reticle stays on screen at any internal resolution. The game truncated a
  coordinate on the way into its UI space, which sent the reticle off screen once the buffer
  was wider than 4095 pixels; the .asi widens those conversions. The cause was found by
  drbermejor's mgs4Ultra120 (https://github.com/drbermejor/mgs4Ultra120).


## Mod compatibility

This mod is meant to share the game with others. Where another mod it recognises changes the
same thing as one of these settings, that mod's setting is used: the tool greys the field out
and shows the other mod's value, and the .asi logs the same. The other mod's files are only
ever read, never written.

Recognised today:
- MGSPatriotFix: Anisotropic Filtering stands aside whenever that mod is installed; Shadow
  Resolution Scale stands aside when its Custom Shadow Resolution is set to anything but 0.

Some mods have a small config file and no tool of their own. When one of those is installed,
the tool grows a tab for it, and only then:
- MGSFPSUnlock by cipherxof (https://github.com/cipherxof/MGSFPSUnlock): the tab edits the
  target frame rate in MGSFPSUnlock.ini.

A save writes just those values back into the mod's file; comments and everything else in it
stay as they were. If the mod's files are sitting somewhere the game does not load from (the
install folder's own scripts folder rather than MGS4\scripts, a common slip), the tab says so.


## Reporting a problem

A report is only useful with a log:

1. In the tool, Troubleshooting tab, tick Debug Logging and save.
2. Launch the game from Steam and reproduce the problem. If it crashes or hangs, stop there.
3. Quit the game (Task Manager if it hung).
4. Attach logs\MGS4Enabler_Game.log from the game's install folder. The log is rewritten on
   every launch, so copy it before starting the game again.

A HUD element in the wrong place on an ultrawide or 4:3 screen: with Debug Logging on and a
Centered or Expanded HUD, press F11 in-game while the element is on screen. A record of every
HUD element being drawn at that moment goes into the log. Take a screenshot at the same moment
(Steam's F12) and attach both.

Issues: https://github.com/BevelTipDrip/MGS4-SSAA-AspectRatio-Enabler/issues


## Credits

ASI loading by Ultimate ASI Loader (ThirteenAG), see UltimateASILoader_LICENSE.md.

Libraries: safetyhook (hooking), Zydis (instruction decoding), spdlog (logging), wxWidgets
(the tool).


## Licence

The public source is under the MIT License.

The ultrawide and 4:3 fix, compiled into the release binaries, is licensed separately under a
credit-required licence: you may use, modify and redistribute it, in your own mods too, but the
fix must be credited wherever it is used or distributed, on the mod page and in the README, with
a link to the GitHub repository. Passing it off as your own ends that permission.
