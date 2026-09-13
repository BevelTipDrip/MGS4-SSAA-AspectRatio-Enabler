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

    // [Graphics] - research knobs. A Lab build of the ASI reads them from this file (and from
    // MGSPWEnabler.lab.settings when the lab marker is present, which then wins); a Release
    // build does not know them. The tool shows them on the Peace Walker window so the knobs
    // can be turned without a research boot.
    constexpr const char* RenderScale = "Render Scale";                        // integer multiple of the 480x272 canvas; 0 = the game's own
    constexpr const char* RenderScaleHundredths = "Render Scale Hundredths";   // experiment: force the two float copies of the scale to this / 100 (Afevis's fractional scale); 0 = off
    constexpr const char* SceneSizeWidth = "Scene Size Width";                 // experiment: full-canvas scene targets created at this size instead of canvas x scale (Afevis's target replacement); 0 = off
    constexpr const char* SceneSizeHeight = "Scene Size Height";
    constexpr const char* WideCanvasUnits = "Wide Canvas Units";               // experiment (private module): the scene target this many canvas units wide, the UI kept at 480; 0 = off
    constexpr const char* InternalSizeWidth = "Internal Size Width";           // post-chain target size; 0 = the scene size
    constexpr const char* InternalSizeHeight = "Internal Size Height";
    constexpr const char* OutputSizeWidth = "Output Size Width";               // the picture the game fits into the window; 0 = the game's table
    constexpr const char* OutputSizeHeight = "Output Size Height";
    constexpr const char* WindowMode = "Window Mode";                          // the game's saved display mode read as this: 0 windowed, 1 fullscreen window, 2 exclusive fullscreen; -1 = leave the saved value
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
    constexpr const char* LogLoadedModules = "Log Loaded Modules";
    constexpr const char* LogDecryptTiming = "Log Decrypt Timing";
    constexpr const char* LogCommandLine = "Log Command Line";
}
