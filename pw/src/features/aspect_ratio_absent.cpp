#include "pch.hpp"
#include "aspect_ratio.hpp"
#include "log.hpp"

// The only body of this entry point: Peace Walker's wide layout is the canvas (canvas.hpp).
namespace AspectRatio
{
    void ApplyFixes()
    {
        if (mgs4e::log::Verbose())
        {
            spdlog::info("PW: UI Aspect: no separate UI aspect fix for Peace Walker; the canvas handles wide and tall displays.");
        }
    }
}
