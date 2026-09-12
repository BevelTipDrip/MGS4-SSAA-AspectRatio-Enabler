#pragma once

#include <array>
#include <string_view>

#include "../features.hpp"
#include "settings_keys.hpp"

// Feature ownership for the Peace Walker build (see ../features.hpp for the idea). The
// generic parts - the known-id table, DisplayName and IsOff - are the MGS4 build's, included
// above; only the claims this build makes are here. None yet: they arrive with the settings
// that patch something another mod may also patch.
namespace mgspwe::features
{
    using mgs4e::features::Ours;
    using mgs4e::features::IsOff;
    using mgs4e::features::DisplayName;

    inline constexpr std::array<Ours, 0> kOurs {};

    inline const Ours* OurClaim(std::string_view section, std::string_view key)
    {
        for (const Ours& o : kOurs)
        {
            if (section == o.section && key == o.key)
            {
                return &o;
            }
        }
        return nullptr;
    }
}
