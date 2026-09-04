#pragma once

// Aspect ratio correction for the game's UI.
//
// The implementation is a separate, private module checked out at external/ultrawide. When it
// is present its own header is the one included here and PFCompanion.vcxproj compiles its
// source; when it is absent this file declares the same interface and aspect_ratio_absent.cpp
// provides an ApplyFixes() that only logs, so the project builds and runs without the ultrawide
// fixes. The settings keys are read either way and are simply inert without the module.
#if __has_include("../../../external/ultrawide/src/fixes/aspect_ratio.hpp")
#include "../../../external/ultrawide/src/fixes/aspect_ratio.hpp"
#else

#include <cstdint>

namespace AspectRatio
{
    void ApplyFixes();

    inline bool bFixUiAspect = false;
    inline bool bFixUiAspectCorners = false;
    inline bool bLogUiChannels = false;
    inline bool bLogLa2Loads = false;
    inline bool bDumpLa2 = false;
    inline uint32_t uAspectTestStrcode = 0;
    inline bool bAspectRuntimeAnchoring = false;
    inline bool bAspectPoolSpread = false;
    inline bool bAspectWorldTracking = false;
    inline bool bAspectTestLadder = false;
    inline bool bMenuMasking = false;
    inline bool bCutsceneMasking = false;
    inline bool bCutsceneFovCompensation = false;
    inline float fFovAdjustment = 1.0f;
}

#endif
