#include "pch.hpp"
#include "aspect_ratio.hpp"
#include "log.hpp"

// Compiled only when external/ultrawide-pw is not checked out (see MGSPWEnabler.vcxproj).
namespace AspectRatio
{
    void ApplyFixes()
    {
        if (mgs4e::log::Verbose())
        {
            spdlog::info("PW: UI Aspect: the Peace Walker ultrawide module (external/ultrawide-pw) is not part of this build.");
        }
    }
}
