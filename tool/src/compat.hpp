#pragma once

#include "manifest_claims.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Mod compatibility, tool side. Detects the mods listed in shared/compat_table.hpp and, for a
// field one of them already controls, supplies the text to show beside the greyed-out field.
// Read-only: the tool never writes to another mod's files.
namespace mgs4e::tool::compat
{
    struct Status
    {
        std::size_t mod = 0;   // index into mgs4e::compat::kMods
        bool asiInstalled = false;
        bool settingsFound = false;
        std::filesystem::path asiPath;
        std::filesystem::path settingsPath;
    };

    // Looks for each known mod's ASI (MGS4\scripts\ or beside mgs4.exe) and settings file.
    void Detect(const std::filesystem::path& gameRoot);

    // One entry per known mod, in kMods order.
    const std::vector<Status>& All();

    // The known mods that are installed, for the tool's note.
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
