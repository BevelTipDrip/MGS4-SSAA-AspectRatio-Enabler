#pragma once

#include "games.hpp"
#include "manifest_claims.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Mod compatibility, tool side. Detects the mods the game's descriptor lists and, for a field
// one of them already controls, supplies the text to show beside the greyed-out field.
// Read-only: the tool never writes to another mod's files. One game per process, so the
// state is process-wide and belongs to the game Detect() was given.
namespace mgs4e::tool::compat
{
    struct Status
    {
        std::size_t mod = 0;   // index into the game's mods
        const char* name = "";
        bool asiInstalled = false;
        bool settingsFound = false;
        std::filesystem::path asiPath;
        std::filesystem::path settingsPath;
    };

    // Looks for each listed mod's ASI (<exeDir>\scripts\ or beside the exe) and settings file,
    // and scans the manifests the game's descriptor says to look for.
    void Detect(const Game& game, const std::filesystem::path& gameRoot);

    // One entry per listed mod, in the game's order.
    const std::vector<Status>& All();

    // The listed mods that are installed, for the tool's note.
    std::vector<const Status*> Installed();

    // What another mod says about one of our fields, when that mod takes precedence.
    struct Override
    {
        const char* modName;   // e.g. "MGSPatriotFix"
        std::string theirKey;  // e.g. "Anisotropic Filtering Level"
        std::string value;     // e.g. "16", or "default" when their key is absent
    };
    std::optional<Override> Overrides(const std::string& section, const std::string& key);

    // The manifest mods' feature claims as found on disk at Detect() (shared/manifest_claims.hpp).
    const std::vector<mgs4e::manifests::Mod>& Manifests();
}
