#include "pch.hpp"
#include "upstream.hpp"

#include "ini.hpp"
#include "overlap_table.hpp"
#include "version.hpp"

namespace pfc::tool::upstream
{
    namespace
    {
        Status g_Status;
        pfc::Ini g_Settings;
    }

    Status Detect(const std::filesystem::path& gameRoot)
    {
        std::error_code ec;
        g_Status = {};

        const std::filesystem::path exeDir = gameRoot / "MGS4";
        for (const std::filesystem::path candidate : { exeDir / "scripts" / PFC_NEIGHBOUR_ASI, exeDir / PFC_NEIGHBOUR_ASI })
        {
            if (std::filesystem::exists(candidate, ec))
            {
                g_Status.asiInstalled = true;
                g_Status.asiPath = candidate;
                break;
            }
        }

        g_Status.settingsPath = gameRoot / PFC_NEIGHBOUR_SETTINGS_FILE;
        g_Status.settingsFound = g_Settings.Load(g_Status.settingsPath);
        return g_Status;
    }

    const Status& Current()
    {
        return g_Status;
    }

    std::optional<std::string> Overrides(const std::string& section, const std::string& key)
    {
        const pfc::overlap::Entry* entry = pfc::overlap::Find(section.c_str(), key.c_str());
        if (!entry)
        {
            return std::nullopt;
        }
        const std::string theirs = g_Settings.Raw(entry->theirSection, entry->theirKey).value_or(std::string());
        if (!pfc::overlap::TheirsWins(*entry, g_Status.asiInstalled, theirs))
        {
            return std::nullopt;
        }
        return std::string(entry->theirKey) + " = " + (theirs.empty() ? "default" : theirs);
    }
}
