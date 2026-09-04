#include "pch.hpp"
#include "aspect_ratio.hpp"
#include "log.hpp"

// Compiled only when external/ultrawide is not checked out (see MGS4Enabler.vcxproj). The
// module's header is then also absent, so the interface declared in aspect_ratio.hpp's
// fallback branch is what the rest of the ASI links against.
namespace AspectRatio
{
    void ApplyFixes()
    {
        if (bFixUiAspect || bFixUiAspectCorners || bAspectPoolSpread)
        {
            spdlog::warn("MGS4: UI Aspect: the ultrawide module (external/ultrawide) is not part of this build; the UI aspect settings do nothing.");
        }
    }
}
