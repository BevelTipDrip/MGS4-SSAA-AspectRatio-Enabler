# MGS Peace Walker SSAA and Aspect Ratio Enabler: Ultrawide

The aspect-ratio work for METAL GEAR SOLID: Peace Walker - Master Collection Version on PC:
HUD groups and overlays placed at the edges of any output aspect, the canvas-sized fades and
overlays widened to the window, cutscenes and character cards left at their 16:9.

This is the Peace Walker part of the ultrawide folder of [MGS4 SSAA and Aspect Ratio Enabler](https://github.com/BevelTipDrip/MGS4-SSAA-AspectRatio-Enabler).
`MGSPWEnabler.vcxproj` compiles `src/fixes/canvas.cpp` from here, and `src/lab/ui_bias.cpp` in
the Lab configuration.

This code is licensed separately from the rest of the repository: free to use and build on,
but the ultrawide and 4:3 fix must be credited directly and visibly to BevelTipDrip. See
`../LICENSE.md`.

## Layout

| Path | What |
| --- | --- |
| `src/fixes/canvas.hpp`, `canvas.cpp` | the wide and tall canvas: the engine's canvas fields set to the display's shape, and the post-quad rewrites that follow it |
| `src/lab/ui_bias.hpp`, `ui_bias.cpp` | the live-editing biases (`bias`, `tbias`) the Lab census calls into: the two `Unmap` rewrites of section 3 of `docs/ui-elements.md` |
| `docs/ui-elements.md` | how the Peace Walker UI is drawn, how an element is identified and moved, the element register, and where this overlaps the MGS4 fix |
| `docs/engine-notes.md` | the engine facts behind the canvas |

The lab that produced the measurements is described in `docs/pw/lab.md` in the main tree (the
draw census and the route replay harness); its scripts are not in this repository.

## Credit

The canvas is built on Afevis's MGSPW-Resolution-Unlocked (MIT): its hook set is the base of
`canvas.cpp`, see `../THIRD-PARTY-NOTICES.md`. Ultrawide and 4:3 HUD, menu and cutscene fix by
BevelTipDrip. Research and measurement by
BevelTipDrip, 2026. Community use is welcome; passing the work off as your own is not.
