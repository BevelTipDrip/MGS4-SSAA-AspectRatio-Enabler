#pragma once

#include <cstddef>
#include <string>

// Mod compatibility, Peace Walker build: the same mechanism as the MGS4 build's
// asi/src/core/compat.cpp over this game's tables (shared/pw/compat_table.hpp) and manifest
// places (shared/pw/manifest_places.hpp). Other mods' files are only ever read.
namespace mgspwe::compat
{
    // Looks for each listed mod's ASI and settings file, then scans the manifests. Called
    // once after our own settings are loaded.
    void Detect();

    // Whether the listed mod at `mod` is installed.
    bool Installed(std::size_t mod);

    // Whether our setting `key` in `section` should stand down because another mod is
    // patching the same thing (a table rule, or an active manifest claim on our feature).
    bool Yields(const char* section, const char* key);

    // The raw value of one of a listed mod's settings, or empty when absent.
    std::string Value(std::size_t mod, const char* theirSection, const char* theirKey);

    // Warns about every feature two or more other mods have on at once.
    void LogOverlaps();
}
