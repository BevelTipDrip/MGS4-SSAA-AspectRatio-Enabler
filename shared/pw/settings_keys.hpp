#pragma once

// The keys in MGSPWEnabler.settings, shared by the Peace Walker ASI and the settings tool so
// the two never disagree on spelling. The MGS4 keys live in ../settings_keys.hpp and are a
// different file on purpose: the two games share no engine, so they share no settings.
namespace mgspwe::keys
{
    constexpr const char* Graphics = "Graphics";
    constexpr const char* Debugging = "Debugging";

    // [Debugging]
    constexpr const char* DebugLogging = "Debug Logging";

    // [Graphics] - the settings of the Peace Walker window (the same shape as the MGS4 one).
    constexpr const char* AspectRatio = "Aspect Ratio";
    constexpr const char* AspectRatio_Display = "Use Display";   // the primary display's shape; 16:9 there means a plain run
    constexpr const char* AspectRatio_16_9 = "16:9";
    constexpr const char* AspectRatio_16_10 = "16:10";
    constexpr const char* AspectRatio_21_9 = "21:9";
    constexpr const char* AspectRatio_32_9 = "32:9";
    constexpr const char* AspectRatio_4_3 = "4:3";
    // One screen (output) and one render list per shape; the ASI reads the one of the selected shape.
    constexpr const char* ScreenResolution16x9 = "Screen Resolution (16:9)";
    constexpr const char* ScreenResolution16x10 = "Screen Resolution (16:10)";
    constexpr const char* ScreenResolution21x9 = "Screen Resolution (21:9)";
    constexpr const char* ScreenResolution32x9 = "Screen Resolution (32:9)";
    constexpr const char* ScreenResolution4x3 = "Screen Resolution (4:3)";
    constexpr const char* RenderResolution16x9 = "Render Resolution (16:9)";
    constexpr const char* RenderResolution16x10 = "Render Resolution (16:10)";
    constexpr const char* RenderResolution21x9 = "Render Resolution (21:9)";
    constexpr const char* RenderResolution32x9 = "Render Resolution (32:9)";
    constexpr const char* RenderResolution4x3 = "Render Resolution (4:3)";
    constexpr const char* WindowMode_Off = "Use Game Setting";
    constexpr const char* WindowMode_Fullscreen = "Fullscreen";   // the game's exclusive mode (2)
    constexpr const char* WindowMode_Borderless = "Borderless";   // a frameless window the size of the display (the game's 0)
    constexpr const char* WindowMode_Windowed = "Windowed";       // a normal window (the game's 1)
    constexpr const char* AnisotropicFiltering = "Anisotropic Filtering";   // true = 16x on every linear sampler (a number 2..16 is accepted too); false = the game's samplers
    constexpr const char* Msaa = "MSAA";
    constexpr const char* Msaa_4x = "4x";
    constexpr const char* Msaa_Off = "Off";

    // [Graphics] - research knobs. A Lab build of the ASI reads them from this file (and from
    // MGSPWEnabler.lab.settings when the lab marker is present, which then wins); a Release
    // build does not know them. The tool shows them on the Peace Walker window so the knobs
    // can be turned without a research boot.
    constexpr const char* RenderScale = "Render Scale";                        // integer multiple of the 480x272 canvas; 0 = the game's own
    constexpr const char* RenderScaleHundredths = "Render Scale Hundredths";   // experiment: force the two float copies of the scale to this / 100 (Afevis's fractional scale); 0 = off
    constexpr const char* SceneSizeWidth = "Scene Size Width";                 // experiment: full-canvas scene targets created at this size instead of canvas x scale (Afevis's target replacement); 0 = off
    constexpr const char* SceneSizeHeight = "Scene Size Height";
    constexpr const char* WideCanvas = "Wide Canvas";                          // the non-16:9 infrastructure (private module): canvas widened to the display aspect, UI centred, effects widened; off = a plain 16:9 run
    constexpr const char* WideCanvasUnits = "Wide Canvas Units";               // canvas width in PSP units when Wide Canvas is on (650 at 21:9, 480 at 4:3); 0 = from the primary display's aspect
    constexpr const char* WideCanvasHeightUnits = "Wide Canvas Height Units";  // canvas height in PSP units (272 at 21:9, 360 at 4:3, 300 at 16:10); 0 = from the primary display's aspect
    constexpr const char* InternalSizeWidth = "Internal Size Width";           // post-chain target size; 0 = the scene size
    constexpr const char* InternalSizeHeight = "Internal Size Height";
    constexpr const char* OutputSizeWidth = "Output Size Width";               // the picture the game fits into the window; 0 = the game's table
    constexpr const char* OutputSizeHeight = "Output Size Height";
    constexpr const char* WindowMode = "Window Mode";                          // one of the WindowMode_ names (a Lab file may still hold 0/1/2/-1)
    constexpr const char* DisableMsaa = "Disable MSAA";
    constexpr const char* GpuLocalTextures = "GPU Local Textures";
    constexpr const char* BackBufferAtInternalSize = "Back Buffer At Internal Size";
    constexpr const char* DrawCensusAtSeconds = "Draw Census At (s)";
    constexpr const char* DrawCensusFrames = "Draw Census Frames";
    constexpr const char* LiveCommands = "Live Commands";
    constexpr const char* TimePasses = "Time Passes";
    constexpr const char* TimeFrames = "Time Frames";
    constexpr const char* DumpShaders = "Dump Shaders";
    constexpr const char* LightHooks = "Light Hooks";
    constexpr const char* FullscreenRefreshFix = "Fullscreen Refresh Rate Fix";   // off: the game's 60/1 request stands in exclusive fullscreen
    constexpr const char* FullscreenResolutionFix = "Fullscreen Resolution Fix";   // off: the game's largest-mode pick and its own buffer size stand
    constexpr const char* PacingLog = "Pacing Log";
    constexpr const char* HitchSampler = "Hitch Sampler";
    constexpr const char* VBlankWaitTimeout = "VBlank Wait Timeout";
    constexpr const char* FrameHandoffWait = "Frame Handoff Wait";
    constexpr const char* VBlankWaitLog = "VBlank Wait Log";
    constexpr const char* FrameSkipGovernor = "Frame Skip Governor";   // Lab: false stops the game raising its vblank wait count when a frame measures long (the doubled frame every second in direct flip)   // Lab: per game-thread vblank wait, the ticks passed and the work time; skipped ticks logged   // Lab: ms the game thread waits for the render thread to take the previous frame before its own frame would be dropped; 0 the game's (drop at once)   // Lab: ms; 0 the game's infinite wait on its vblank event; 1 recovers a lost wakeup within a millisecond   // Lab: when the render thread has waited 20 ms for a frame, sample every other thread once (module+offset, game return addresses, current wait)   // Lab: the frame pacing instrumentation (import hooks on Sleep/waits, tick counter, 5 s summaries)
    constexpr const char* FramePacing = "Frame Pacing";
    constexpr const char* FramePacing_Game = "Game";
    constexpr const char* FramePacing_Display = "Display";
    constexpr const char* FramePacing_VBlank = "VBlank";   // Lab: "Game" or "Display"
    constexpr const char* BusyWaitFix = "Busy Wait Fix";   // 0 off, 1 the render thread's handoff spin, 2 also the ticker's sub-millisecond tail, 3 also the message pump
    constexpr const char* BusyWaitFix_Off = "Off";
    constexpr const char* BusyWaitFix_Render = "Render thread";
    constexpr const char* BusyWaitFix_Ticker = "Render thread and ticker";
    constexpr const char* BusyWaitFix_All = "Everything";
    constexpr const char* YieldLockBeforePresent = "Yield Lock Before Present";   // ms of margin: the display lock is given up until shortly before the flip is due, then taken back so Present still does the pacing
    constexpr const char* VBlankOutsideLock = "Wait For VBlank Outside The Lock";   // the vsync wait is done on the display's vertical blank with the game's display lock released, then the lock is taken back and Present is called
    constexpr const char* FreeLockDuringPresent = "Free Lock During Present";   // the render thread holds the game's display critical section across Present; released around the wait so the game thread can record the next frame
    constexpr const char* StateObjectCache = "State Object Cache";   // 0 none, 1 depth-stencil, 2 samplers, 3 both: hand back the state object the game already asked for instead of calling the device again
    constexpr const char* PresentSync = "Present Sync";
    constexpr const char* SwapChainBuffers = "Swap Chain Buffers";
    constexpr const char* AllowTearing = "Allow Tearing";
    constexpr const char* PresentNoWait = "Present Without Blocking";   // Lab: Present asks not to block and is retried, so the Direct3D device lock is not held across the vsync wait   // Lab: the flip chain's tearing-allowed mode: Present never waits for the display   // Lab: 0 the game's (2), else the flip chain's buffer count   // Lab: -1 the game's (1), else the sync interval passed to Present
    constexpr const char* LogLoadedModules = "Log Loaded Modules";
    constexpr const char* LogDecryptTiming = "Log Decrypt Timing";
    constexpr const char* LogCommandLine = "Log Command Line";
}
