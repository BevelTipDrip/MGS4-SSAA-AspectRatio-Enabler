#pragma once

#include <filesystem>
#include <optional>
#include <string>

// MGSPatriotFix, the mod PF Companion runs alongside. Read-only: the tool never writes to
// its files. Where both mods patch the same thing (shared/overlap_table.hpp) the field is
// greyed out here and the value MGSPatriotFix is using is shown beside it.
namespace pfc::tool::upstream
{
    struct Status
    {
        bool asiInstalled = false;
        bool settingsFound = false;
        std::filesystem::path asiPath;
        std::filesystem::path settingsPath;
    };

    // Looks for the ASI (MGS4\scripts\ or beside mgs4.exe) and the settings file.
    Status Detect(const std::filesystem::path& gameRoot);
    const Status& Current();

    // For one of our fields: the text to show when MGSPatriotFix takes precedence
    // ("Anisotropic Filtering Level = 16"), or nothing when the field is ours to set.
    std::optional<std::string> Overrides(const std::string& section, const std::string& key);
}
