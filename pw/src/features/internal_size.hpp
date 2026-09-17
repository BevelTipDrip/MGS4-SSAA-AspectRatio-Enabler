#pragma once

#include "lab.hpp"

// The rendering settings: render scale, canvas, output size, window mode, and the knobs the
// Direct3D hooks apply (MSAA, frame textures in video memory, anisotropic filtering).
//
// The game renders the whole frame (world and HUD) into one internal target whose size is an
// immediate constant chosen by the launcher's -resolution flag (1440x816 or 1920x1088), then
// blits it, through its own fit, into the output size from a table chosen by -upscale. Apply()
// rewrites those immediates and the table at init, so the internal target can be any integer
// multiple of the canvas and the output the window itself: the game's final blit then
// downsamples, with no oversized window. The experiments below stay Lab-only (compile-time
// constants in a Release build, so their code and strings drop out).
namespace InternalSize
{
    inline int iRenderScale = 0;      // integer canvas multiple k (game: 3 or 4); 0: leave alone. Sets the scale getter and, unless given, the internal size 480k x 272k
    MGS4E_LAB_SWITCH(int, iRenderScaleHundredths, 0); // experiment: the float copies of the scale forced to this / 100 at +596E5 and +5B015 (what Afevis's patch does); 0 = off
    inline bool bWideCanvas = false;    // the non-16:9 infrastructure (private module): canvas widened to the display aspect, UI centred, effects widened
    inline int iWideCanvasUnits = 0;    // canvas width in PSP units when bWideCanvas (650 at 21:9, 480 at 4:3); 0 = from the primary display's aspect
    inline int iWideCanvasHeightUnits = 0; // canvas height in PSP units (272 at 21:9, 360 at 4:3, 300 at 16:10); 0 = from the primary display's aspect
    MGS4E_LAB_SWITCH(int, iSceneWidth, 0);         // experiment: the full-canvas scene targets (480x272 x scale) created at this size instead, as Afevis's target replacement does; 0 = off
    MGS4E_LAB_SWITCH(int, iSceneHeight, 0);
    MGS4E_LAB_SWITCH(int, iInternalWidth, 0);    // 0: derive from the render scale, or leave the game's immediates alone
    MGS4E_LAB_SWITCH(int, iInternalHeight, 0);
    inline int iOutputWidth = 0;      // 0: leave the output table alone
    inline int iOutputHeight = 0;
    inline int iWindowMode = -1;        // written into the game's saved display mode (settings id 3) once per run, the game's numbering: 0 Borderless, 1 Windowed, 2 Fullscreen (exclusive); -1 = leave. The saved values are logged either way
    MGS4E_LAB_SWITCH(bool, bBackBufferAtInternal, false);
    MGS4E_LAB_SWITCH(int, iSwapChainBuffers, 0);   // 0 the game's flip chain (2 buffers: one frame in flight, Present blocks on the display); 3 gives Present a frame of slack // swap chain buffers at the internal size (flip model stretches them onto the window) and the fit told the picture is the whole buffer
    inline bool bFullscreenRefreshFix = true;      // exclusive fullscreen asks for the display's current refresh rate instead of the game's 60/1 (render_policy.cpp)
    inline bool bFullscreenResolutionFix = true;   // exclusive fullscreen runs at the selected screen resolution instead of the monitor's largest mode (the +1A25B hook and the buffer guard)
    inline bool bGpuLocalTextures = false;
    // Below this width a texture keeps its CPU write access. 1024 is what shipped and is measured
    // safe; lower values cover more of the traffic to system memory, which matters on a narrow or
    // older PCI Express link, at the risk of stripping one the game still writes to. The map
    // failure counter in the census is the check.
    MGS4E_LAB_SWITCH(int, iGpuLocalMinWidth, 1024); // strip CPU write access from large default textures so GPU copies into them stay in video memory (render_policy.cpp)
    inline bool bDisableMsaa = false; // scene targets created single-sampled; the game's resolve becomes a copy (render_policy.cpp)
    inline int iAnisotropy = 0;       // 2..16: every linear sampler anisotropic at that level; 0 = the game's samplers (render_policy.cpp)

    void Apply();
    // The size the census gives the swap chain when bBackBufferAtInternal is on (0 = leave).
    int BackBufferWidth();
    int BackBufferHeight();
}
