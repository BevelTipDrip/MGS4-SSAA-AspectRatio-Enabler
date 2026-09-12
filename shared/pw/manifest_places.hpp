#pragma once

#include "../manifest_claims.hpp"

// Where the Peace Walker build looks for other mods' manifests, and what they are called.
// The game's loader reads .asi files from mgspw\ and its scripts/plugins/update subfolders;
// the same names one level up are a zip extracted too high (found, reported, not loadable).
namespace mgspwe::manifests
{
    inline constexpr const char* kSuffix = ".MGSPWEnabler.ini";

    inline constexpr mgs4e::manifests::Place kPlaces[] = {
        { "mgspw", true }, { "mgspw\\scripts", true }, { "mgspw\\plugins", true }, { "mgspw\\update", true },
        { ".", false }, { "scripts", false }, { "plugins", false }, { "update", false },
    };
}
