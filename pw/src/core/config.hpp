#pragma once

#include <filesystem>

#include "lab.hpp"

// MGSPWEnabler.settings, and the lab file that replaces it for research boots. Same contract
// as the MGS4 build's config: Load() reads the file once into the feature namespaces; a
// missing file is not an error, every setting has a default that leaves the game alone.
namespace mgspwe::config
{
    void Load();

    // Whether this boot is a lab run (a fresh MGSPWEnabler.lab marker next to a
    // MGSPWEnabler.lab.settings, consumed as it is read). A release build has no lab mode.
#if MGS4E_LAB_BUILD
    bool LabMode();
#else
    constexpr bool LabMode() { return false; }
#endif

    // The settings file that was (or would have been) read.
    const std::filesystem::path& File();
}
