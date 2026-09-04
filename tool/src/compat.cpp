#include "pch.hpp"
#include "compat.hpp"

#include "ini.hpp"
#include "compat_table.hpp"
#include "version.hpp"

namespace mgs4e::tool::compat
{
    namespace
    {
        std::vector<Status> g_Status;
        std::vector<mgs4e::Ini> g_Settings;
    }

    void Detect(const std::filesystem::path& gameRoot)
    {
        using mgs4e::compat::kModCount;
        using mgs4e::compat::kMods;

        std::error_code ec;
        g_Status.assign(kModCount, Status{});
        g_Settings.assign(kModCount, mgs4e::Ini{});

        const std::filesystem::path exeDir = gameRoot / "MGS4";
        for (std::size_t i = 0; i < kModCount; ++i)
        {
            Status& s = g_Status[i];
            s.mod = i;
            for (const std::filesystem::path candidate : { exeDir / "scripts" / kMods[i].asiFile, exeDir / kMods[i].asiFile })
            {
                if (std::filesystem::exists(candidate, ec))
                {
                    s.asiInstalled = true;
                    s.asiPath = candidate;
                    break;
                }
            }
            s.settingsPath = gameRoot / kMods[i].settingsFile;
            s.settingsFound = g_Settings[i].Load(s.settingsPath);
        }
    }

    const std::vector<Status>& All()
    {
        return g_Status;
    }

    std::vector<const Status*> Installed()
    {
        std::vector<const Status*> out;
        for (const Status& s : g_Status)
        {
            if (s.asiInstalled)
            {
                out.push_back(&s);
            }
        }
        return out;
    }

    std::optional<Override> Overrides(const std::string& section, const std::string& key)
    {
        const mgs4e::compat::Entry* entry = mgs4e::compat::Find(section.c_str(), key.c_str());
        if (!entry || entry->mod >= g_Status.size())
        {
            return std::nullopt;
        }
        const std::string theirs = g_Settings[entry->mod].Raw(entry->theirSection, entry->theirKey).value_or(std::string());
        if (!mgs4e::compat::TheirsWins(*entry, g_Status[entry->mod].asiInstalled, theirs))
        {
            return std::nullopt;
        }
        return Override{ mgs4e::compat::kMods[entry->mod].name, entry->theirKey, theirs.empty() ? "default" : theirs };
    }
}
