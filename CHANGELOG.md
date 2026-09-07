# Changelog

Every release gets an entry here before it is packaged; `build\package.ps1` refuses to build a
zip whose version has no heading below. The text is written to be pasted into the Nexus Mods
changelog box as is.

## 0.0.5 (2026-09-07)

Fixes
- Upload scanners flagged 0.0.4: its windowed-mode fix found the game window by enumerating windows, which is how an injector finds a target. The window is now taken from the game's own swap chain and nothing enumerates. Same behaviour, clean import table.
- One spurious swap-chain resize at start-up in windowed mode is gone with it.
- An override of one shape on a display of another, both non-16:9 (a 16:10 or 21:9 setting on a 4:3 monitor, say), came out squeezed: the renderer's 16:9 band was worked out from the window, but the engine derives it from the display and scales it into the window. Now matched the engine's way. 4:3 on 4:3 and every setting on a 16:9 display were already right.

## 0.0.4 (2026-09-07)

Fixes
- Windowed mode with the game's own resolution set below the mod's: black screen on DirectX 11, misaligned picture on DirectX 12. The engine keeps a second copy of its resolution inside the renderer, which the mod never wrote; it now follows the window on every frame. Changing the resolution in-game while running works too.
- An override larger than the screen (or the window) is now shrunk to fit, keeping its shape, instead of running off the display.

New
- 16:10 aspect ratio, with its own resolution list. Same treatment as 4:3, with thinner bars.
- Fuller resolution lists for every shape: 16:9 up to 8K, 16:10 from 1280x800 to 3840x2400, the common 21:9 and 32:9 panels, 4:3 from 640x480 to 3200x2400.

Notes
- Windowed mode stutters compared with fullscreen, with or without this mod (confirmed with the loader removed entirely): the game presents through the desktop compositor there. Turning the game's own V-Sync off helps a lot; fullscreen is the smooth option.

## 0.0.3 (2026-09-06)

Fixes
- Codec calls: the caller's portrait no longer takes the shape of the window (narrow on ultrawide, wide on 4:3). It keeps its 16:9 framing inside the codec, and the codec stays masked like the other menus.
- Pre-rendered videos (the opening's TV commercials and the like) were stretched over the whole window. They now play pillarboxed on ultrawide and letterboxed on 4:3.
- The difficulty select, load list and options screens were stretched across the full window while the main menu next to them was not. They now follow the same 16:9 layout as everything else.
- DirectX 12: when the chosen resolution was smaller than the display, the picture sat unscaled in the top-left corner. It is now scaled and centred like on DirectX 11, and the area around it is cleared.
- Every fix above verified on DirectX 11 and DirectX 12, on native 4:3 and native ultrawide displays.

Known issues (all ultrawide / 4:3 only, none affect 16:9)
- The in-game options menu (Nomad interior) still scales like the codec used to.
- 3D renders of items, weapons and the camo screen are not corrected yet.
- The map render is not corrected yet.
- Subtitles in gameplay and cutscenes are stretched.

## 0.0.2 (2026-09-05)

Fixes
- 4:3 and ultrawide pictures were being squeezed into a 16:9 band by the renderer's final pass, on both DirectX 11 and 12, with garbage in the corners. The pass now covers the whole window.
- The picture never filled the display when the chosen resolution differed from the desktop: the game sizes its swap chain to the largest display mode the monitor lists, and under DirectX 12 switches the display to that mode. With a Window Aspect Ratio set, the mod now keeps the desktop's mode, sizes the chain to the window and centres the picture.
- Menu and cutscene masking now works on DirectX 11 (it was DirectX 12 only).
- Anisotropic filtering now applies on DirectX 11.
- Menus at 21:9 and 4:3 stuttered badly with the Expanded HUD; fixed (a per-node memory query in the HUD attribution, about half of every second spent in menus).

## 0.0.1 (2026-09-04)

First release.

Image quality
- Internal resolution scaling up to 400 % (supersampling), independent of the window resolution.
- Shadow map resolution scaling and shadow filter sample count.
- Anisotropic filtering up to 16x.
- FXAA on/off and quality level.

Aspect ratio
- Window resolution and aspect ratio selection (16:9, 21:9, 32:9, 4:3), independent of the launcher.
- Three HUD modes: Stretched (stock), Centered and Expanded. Centered gives every widget its proper shape in a 16:9 area; Expanded also moves the life bar, camo meter, weapon and item panels to the real screen edges.
- Labels that follow things in the world (pickups, Solid Eye stat blocks, targeting icons) stay on their objects.
- 16:9 masking for the title, menus, codec, loading screens and cutscenes, each its own switch.
- Cutscene FOV compensation for wide windows, and a gameplay FOV adjustment (50 to 200 %).

Tool
- A settings tool that writes one file, MGS4Enabler.settings. The .asi runs without it.
- A tab for MGSFPSUnlock's target frame rate when that mod is installed.
- Mod compatibility: where another known mod controls the same setting, that mod's value wins and the tool says so.
