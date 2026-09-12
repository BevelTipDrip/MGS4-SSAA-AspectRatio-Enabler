#pragma once

// Aspect ratio correction for Peace Walker's UI.
//
// As in the MGS4 build, the implementation is a separate, private module, checked out at
// external/ultrawide (under pw/). When it is present its own header is included here and
// MGSPWEnabler.vcxproj compiles its source; when absent this file declares the same
// interface and aspect_ratio_absent.cpp provides an ApplyFixes() that only logs.
#if __has_include("../../../external/ultrawide/pw/src/fixes/aspect_ratio.hpp")
#include "../../../external/ultrawide/pw/src/fixes/aspect_ratio.hpp"
#else

namespace AspectRatio
{
    void ApplyFixes();
}

#endif
