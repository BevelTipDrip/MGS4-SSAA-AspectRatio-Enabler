#pragma once

#include <cstddef>
#include <string>

// Mod compatibility: detecting the other mods listed in shared/compat_table.hpp and standing
// down where one of them already patches what one of our settings would.
//
// The loader runs `MGS4\scripts` alphabetically and each ASI installs its hooks synchronously,
// so by the time we run, any mod that sorts before us has its patches in place. Their settings
// files are only ever read.
namespace mgs4e::compat
{
    // Looks for each known mod's ASI in the process (or on disk, for the file-only case) and
    // reads its settings file if there is one. Called once after our own settings are loaded.
    void Detect();

    // Whether the known mod at `mod` (index into kMods) is installed.
    bool Installed(std::size_t mod);

    // Whether our setting `key` in `section` should stand down because a known mod is
    // patching the same thing. Logs the decision the first time it says yes for a key.
    bool Yields(const char* section, const char* key);

    // The raw value of one of a known mod's settings, or empty when absent.
    std::string Value(std::size_t mod, const char* theirSection, const char* theirKey);
}
