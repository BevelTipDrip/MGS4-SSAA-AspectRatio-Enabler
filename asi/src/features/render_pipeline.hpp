#pragma once

namespace RenderPipeline
{
    void ApplyFixes();

    // Multiplier applied to the engine's internal render buffer (render.bufferSizeX/Y).
    // The window and swapchain keep the game's configured resolution, so the engine
    // renders internally at a higher resolution and scales down on output, i.e.
    // supersampling. 1.0 leaves the game's own resolution untouched.
    inline double fInternalResolutionScale = 1.0;

    // Multiplier applied to the engine's shadow map buffer size, which the scalability
    // config supplies as "ShadowBufferSize". The shadow atlas is allocated as
    // width x 2*width, so this raises both. 1.0 leaves the game's own value alone.
    inline double fShadowResolutionScale = 1.0;

    // Shadow filter taps. More samples soften a shadow's edge rather than sharpening it, so
    // this is separate from the resolution scale; the game's highest preset supplies 7.
    // 0 leaves the engine's own value alone.
    inline int iShadowSampleCount = 0;

    // Maximum anisotropy forced on samplers that already request anisotropic filtering.
    // The game's highest texture preset asks for 8. 0 leaves samplers untouched.
    inline int iAnisotropicFiltering = 0;

    // FXAA on or off. The game has its own toggle but only in its menu, and it needs a
    // restart either way. -1 leaves the game's own setting alone.
    inline int iFxaaOverride = -1;

    // FXAA quality: Slow(0), Medium(1), Fast(2) - slower being higher quality. The engine
    // ships 1 and exposes this nowhere. -1 leaves it alone.
    inline int iFxaaQuality = -1;

    // Overrides the engine's window / swapchain resolution (render.windowSizeX/Y), which
    // is the game's actual output size. Independent of the internal render buffer, so a
    // smaller window can still be fed by a higher internal resolution.
    inline bool bOverrideWindowSize = false;
    inline int iWindowSizeX = 0;
    inline int iWindowSizeY = 0;

    // The engine's render size globals, resolved when the hook installs. Other fixes
    // (e.g. a future shadow resolution fix) can read the live values through these.
    inline int* pInternalResX = nullptr;
    inline int* pInternalResY = nullptr;
    inline int* pWindowResX = nullptr;
    inline int* pWindowResY = nullptr;

    // Maximum internal render buffer width. A flat 8K ceiling.
    //
    // This used to be 4092, and 6720 with the UI shader workaround, because the engine narrowed
    // the aiming reticle's screen coordinate to 16 bits: at 1/16px precision the largest value
    // it could hold was 2047.94px, so a reticle at width/2 wrapped once the buffer passed 4095
    // wide and the quad rasterised off screen. The shader workaround detected the wrap and added
    // one back, which bought headroom but not much - the sniper scope's vignette sits on a screen
    // edge by construction and the crosshair's arms spread outward from centre, so past about
    // 6720 no single threshold could separate a wrapped coordinate from a legitimate one.
    //
    // Widening those conversions removes the wrap at its source (see graphics_settings.cpp), so
    // neither ceiling applies any more. 8192 is chosen as a deliberate limit rather than a
    // derived one: 8K is far past the point of visible return for supersampling, and the cost
    // grows with the square of the scale - a 7680x4320 buffer is already 16x the pixels of 1080p.
    // The practical limits past here are VRAM and frame time, which no fixed number can predict.
    inline int iMaxInternalBufferWidth = 8192;

    // Removes the 16-bit truncation that loses the aiming reticle above a 4095-wide render
    // buffer, by patching the four instructions that discard the top half of the coordinate.
    //
    // The engine holds the reticle's screen position in a 32-bit register but sign-extends only
    // the low 16 bits (`movsx edx, cx`), so a coordinate past 32767 in 1/16px - i.e. any buffer
    // wider than 4095 - comes out negative and the quad rasterises off screen. Keeping the full
    // 32 bits fixes it at the source.
    //
    // This replaced a vertex shader workaround that detected the damage afterwards and added a
    // wrap back; that approach is gone, since the wrap no longer happens.
    inline bool bFixReticleTruncation = true;

    // Diagnostic: reports every place a converted coordinate is narrowed to 16 bits, whether or
    // not it is scaled by a render size global.
    //
    // The shipped fix deliberately only widens narrowings it can prove are screen coordinates,
    // by requiring the value be divided by render.bufferSizeX/Y. That is the right constraint for
    // something that patches code, but it means any narrowing reached by a different route is
    // invisible. This lists them all so the difference can be examined rather than assumed - the
    // aiming reticle was the symptom people noticed, not necessarily the only one.
    inline bool bScanNarrowingConversions = false;

    // Diagnostics: loads PIX's capture library before the renderer starts, so frames can be
    // captured from inside the process with F10. The game refuses to run when launched by a
    // capture tool directly, and D3D12 cannot be hooked once the device exists, so this is
    // the only way to get a GPU capture out of it. Disables our own D3D12 hooks while on.

    inline bool bRedirectUiCoordinateSpace = false;

    // Diagnostic: disassembles a range of mgs4.exe into the log at startup. RVA as a hex
    // string ("4E14E0"); empty disables. The .text section is encrypted on disk, so this is
    // the practical way to read the game's code.
    inline std::string sDisassembleRva = "";
    inline int iDisassembleBytes = 512;

    // Diagnostic: logs the value at each of a comma-separated list of hex RVAs, as float and
    // as int32. For reading the constants an instruction multiplies by.
    inline std::string sDumpFloats = "";

    // Diagnostic: finds an ASCII string in the image and logs every instruction referencing it.
    // Config keys are strings, so this locates the code that reads a given setting.
    inline std::string sFindString = "";

    // Diagnostic: every instruction addressing a given struct offset, as a hex string. Finds
    // all users of a known field without guessing at instruction patterns.
    inline std::string sFindDisplacement = "";

    // Diagnostic: disassembles all of .text looking for code that reads both a render size
    // global and the 16.0f constant - the signature of a screen-pixel to fixed-point
    // conversion, which is what the dynamic HUD elements overflow.
    inline bool bScanFixedPointSites = false;

    // Diagnostic: also log the disassembly of each cluster the scan reports.
    inline bool bScanClusterDisassembly = false;

    // Diagnostic: hooks the fixed-point conversion at mgs4.exe+4F9F67 and logs the floats it
    // converts, to establish whether they are screen pixels (which wrap) or virtual.
    inline bool bFixedPointProbe = false;

    // Diagnostic: hardware execute breakpoints on up to four comma-separated hex RVAs. Watches
    // instructions without modifying them, which mid hooks cannot do safely for short stores.
    inline std::string sBreakpointRvas = "";

    // Diagnostic: on F9, search memory for an already-wrapped centre-screen coordinate and set
    // hardware write watches on it, so the code that writes it identifies itself.
    inline bool bWatchWrappedValue = false;

    // Diagnostic: hardware watch on the render size globals, armed as soon as they resolve, to
    // log every distinct instruction that reads them - including during load, when the UI
    // geometry that overflows is built.
    inline bool bWatchRenderSizeReads = false;

    // Diagnostic: watch the float copies of the render size instead of the global itself, for
    // code that works from a cached value and so never touches the global.
    inline bool bWatchCachedRenderSize = false;

    // Diagnostic: PAGE_GUARD the whole allocation holding the wrapped coordinate and log the
    // instruction behind each write fault - covers the entire buffer, unlike a debug register.
    inline bool bGuardWrappedRegion = false;

    // Diagnostic: guard the engine heap and record every write, so the one that creates the
    // wrapped coordinate can be looked up afterwards by address.
    inline bool bGuardRecordHeapWrites = false;

    // Diagnostic: plants an INT3 at every candidate fixed-point conversion site, so all of
    // them can be observed at once instead of four at a time with debug registers.
    inline bool bTrapFixedPointSites = false;

    // Diagnostic: on F9, dump the memory around a wrapped coordinate so its structure can be
    // read directly instead of assumed.
    inline bool bDumpWrappedContext = false;

    // Diagnostic: guard each D3D12 upload buffer at creation so the first CPU write to it
    // identifies itself - the only way to catch a value written once, before its address
    // can be known.
    inline bool bGuardUploadBuffers = false;

    // Diagnostic: find the wrapped coordinate inside a staging copy and watch that address.
    // Staging is rewritten every frame, so unlike the vertex pool a watch on it will fire.
    inline bool bWatchStagingWrites = false;

    // Diagnostic: F8 toggles the render buffer globals between buffer and window size while
    // the game runs, to see whether the UI's NDC conversion reads them live or a cached copy.
    inline bool bUiCoordinateProbe = false;

    // Bisection over the candidate sites for the UI's NDC divisor. Patches the half-open
    // range [start, start+count) of the candidate list; count 0 patches nothing.
    inline int iUiNdcSiteStart = 0;
    inline int iUiNdcSiteCount = 0;

    inline bool bEnablePixCapture = false;

    // How many frames each PIX capture spans. One Present-to-Present interval on this game
    // can contain nothing but the final blit, with the scene work landing in a neighbouring
    // interval, so a single-frame capture can come back with no draw calls at all.
    inline int iPixCaptureFrames = 5;

    // Diagnostic: hooks the UI layout converter (logical 1280x720 -> output pixels,
    // mgs4.exe+439810) and logs the rectangles flowing through it with their callers.
    // Answers whether the converter runs per widget or per sub-quad, which decides
    // whether an anchored aspect-ratio remap is viable there. Logging only.
    inline bool bLogUiLayout = false;

    // Diagnostic: hardware write watches on two known anchor slots in the UI instance pool -
    // one static widget (the ration box), one dynamic element (the aiming reticle) - so the
    // code that produces each kind of anchor names itself. The foundation of the
    // producer-based aspect positioning (the ultrawide module).
    inline bool bWatchAnchorProducers = false;

    // Arms one of the producer watches (up to two). Called from the aspect-ratio upload hook
    // once it has located a target block in the live pool.
    void ArmProducerWatch(uintptr_t address, const char* what);

    // Diagnostics: logs every viewport and scissor rect the engine sets, flagging any that
    // are empty. The reticle's draw calls are issued at every render resolution but stop
    // producing output above a 4095-wide buffer, so the pass state is the remaining suspect.
    inline bool bLogViewports = false;

    // Diagnostics: hooks D3D12 resource creation to log every render target allocation
    // with the mgs4.exe call sites that requested it, and watches the render size
    // globals for changes. Off by default; only useful when investigating the renderer.
    inline bool bLogRenderTargetAllocations = false;
}
