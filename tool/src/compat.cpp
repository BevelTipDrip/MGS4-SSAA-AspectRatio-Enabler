#include "pch.hpp"
#include "compat.hpp"

#include "ini.hpp"
#include "compat_table.hpp"
#include "features.hpp"

namespace mgs4e::tool::compat
{
    namespace
    {
        const Game* g_Game = nullptr;
        std::vector<Status> g_Status;
        std::vector<mgs4e::Ini> g_Settings;
        std::vector<mgs4e::manifests::Mod> g_Manifests;
    }

    const std::vector<mgs4e::manifests::Mod>& Manifests()
    {
        return g_Manifests;
    }

    void Detect(const Game& game, const std::filesystem::path& gameRoot)
    {
        g_Game = &game;
        std::error_code ec;
        g_Status.assign(game.mods.size(), Status{});
        g_Settings.assign(game.mods.size(), mgs4e::Ini{});

        const std::filesystem::path exeDir = gameRoot / game.exeDir;
        for (std::size_t i = 0; i < game.mods.size(); ++i)
        {
            Status& s = g_Status[i];
            s.mod = i;
            s.name = game.mods[i].name;
            for (const std::filesystem::path candidate : { exeDir / "scripts" / game.mods[i].asiFile, exeDir / game.mods[i].asiFile })
            {
                if (std::filesystem::exists(candidate, ec))
                {
                    s.asiInstalled = true;
                    s.asiPath = candidate;
                    break;
                }
            }
            s.settingsPath = gameRoot / game.mods[i].settingsFile;
            s.settingsFound = g_Settings[i].Load(s.settingsPath);
        }
        g_Manifests = mgs4e::manifests::Scan(gameRoot, std::string(game.name) + ".settings", game.manifestSuffix, game.places);
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
        if (!g_Game)
        {
            return std::nullopt;
        }
        if (const mgs4e::compat::Entry* entry = g_Game->Find(section.c_str(), key.c_str()); entry && entry->mod < g_Status.size())
        {
            const std::string theirs = g_Settings[entry->mod].Raw(entry->theirSection, entry->theirKey).value_or(std::string());
            if (mgs4e::compat::TheirsWins(*entry, g_Status[entry->mod].asiInstalled, theirs))
            {
                return Override{ g_Game->mods[entry->mod].name, entry->theirKey, theirs.empty() ? "default" : theirs };
            }
        }
        // Only an always-on claim greys our field out for good: a value-gated one the user
        // can turn off from its own tab, so it is reported as an overlap instead (ui.cpp).
        if (const mgs4e::features::Ours* ours = g_Game->OurClaim(section, key))
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
