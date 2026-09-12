# MGS Peace Walker SSAA and Aspect Ratio Enabler: Ultrawide

The aspect-ratio work for METAL GEAR SOLID: Peace Walker - Master Collection Version on PC:
HUD groups and overlays placed at the edges of any output aspect, the canvas-sized fades and
overlays widened to the window, cutscenes and character cards left at their 16:9.

This is the Peace Walker part of the private ultrawide module of [MGS4 SSAA and Aspect Ratio Enabler](https://github.com/BevelTipDrip/MGS4-SSAA-AspectRatio-Enabler):
the same repository as the MGS4 fix, under `pw/`. `MGSPWEnabler.asi` builds with or without
it: when the module is checked out at `external/ultrawide`, MSBuild compiles the sources under
`pw/src` from here; when it is absent, stubs in the public tree log that the module is not
present. Nothing in the public tree describes how the fix works or carries its values.

This code is licensed separately from the public repository: free to use and build on, but the
ultrawide and 4:3 fix must be credited directly and visibly to BevelTipDrip. See `LICENSE.md`.

## Layout

| Path | What |
| --- | --- |
| `docs/ui-elements.md` | how the Peace Walker UI is drawn, how an element is identified and moved, the element register, and where this overlaps the MGS4 module |
| `src/lab/ui_bias.hpp`, `ui_bias.cpp` | the live-editing biases (`bias`, `tbias`) the public Lab census calls into when this module is present: the two `Unmap` rewrites of section 3 of the doc |
| `src/fixes/aspect_ratio.cpp` | the shipped fix (not written yet; the method is measured and documented) |

The lab that produced the measurements is public: `docs/pw/lab.md` in the main repository
(the draw census and the route replay harness); the live-editing commands live here.

## Credit

Research and measurement by BevelTipDrip, 2026. Community use is welcome; passing the work off
as your own is not.
