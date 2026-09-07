#include "pch.hpp"
#include "compat.hpp"

#include "ini.hpp"
#include "compat_table.hpp"
#include "features.hpp"
#include "version.hpp"

namespace mgs4e::tool::compat
{
    namespace
    {
        std::vector<Status> g_Status;
        std::vector<mgs4e::Ini> g_Settings;
        std::vector<mgs4e::manifests::Mod> g_Manifests;
    }

    const std::vector<mgs4e::manifests::Mod>& Manifests()
    {
        return g_Manifests;
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
        g_Manifests = mgs4e::manifests::Scan(gameRoot, std::string(MGS4E_NAME) + ".settings");
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
        if (const mgs4e::compat::Entry* entry = mgs4e::compat::Find(section.c_str(), key.c_str()); entry && entry->mod < g_Status.size())
        {
            const std::string theirs = g_Settings[entry->mod].Raw(entry->theirSection, entry->theirKey).value_or(std::string());
            if (mgs4e::compat::TheirsWins(*entry, g_Status[entry->mod].asiInstalled, theirs))
            {
                return Override{ mgs4e::compat::kMods[entry->mod].name, entry->theirKey, theirs.empty() ? "default" : theirs };
            }
        }
        // Only an always-on claim greys our field out for good: a value-gated one the user
        // can turn off from its own tab, so it is reported as an overlap instead (ui.cpp).
        if (const mgs4e::features::Ours* ours = mgs4e::features::OurClaim(section, key))
        {
            for (const auto& m : g_Manifests)
            {
                for (const auto& c : m.claims)
                {
                    if (c.active && c.alwaysOn && c.feature == ours->feature)
                    {
                        return Override{ m.name.c_str(), c.key, "always on" };
                    }
                }
            }
        }
        return std::nullopt;
    }
}
