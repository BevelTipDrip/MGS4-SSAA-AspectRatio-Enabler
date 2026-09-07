#include "pch.hpp"
#include "compat.hpp"

#include "game.hpp"
#include "log.hpp"
#include "ini.hpp"
#include "compat_table.hpp"
#include "features.hpp"
#include "manifest_claims.hpp"
#include "version.hpp"

namespace mgs4e::compat
{
    namespace
    {
        struct State
        {
            bool installed = false;
            mgs4e::Ini settings;
        };
        State g_Mods[kModCount];
        std::vector<mgs4e::manifests::Mod> g_Manifests;
    }

    void Detect()
    {
        for (std::size_t i = 0; i < kModCount; ++i)
        {
            const Mod& mod = kMods[i];
            State& state = g_Mods[i];

            // The module handle is the authoritative answer: it is what actually patched the
            // game. The on-disk check only covers a loader that has not got to it yet, which
            // does not happen with the alphabetical order, but costs nothing to allow for.
            const std::wstring asiName(mod.asiFile, mod.asiFile + std::strlen(mod.asiFile));
            state.installed = GetModuleHandleW(asiName.c_str()) != nullptr;
            if (!state.installed)
            {
                std::error_code ec;
                const std::filesystem::path scripts = mgs4e::game::ExePath().parent_path() / "scripts" / mod.asiFile;
                const std::filesystem::path beside = mgs4e::game::ExePath().parent_path() / mod.asiFile;
                state.installed = std::filesystem::exists(scripts, ec) || std::filesystem::exists(beside, ec);
            }

            const bool haveSettings = state.settings.Load(mgs4e::game::Root() / mod.settingsFile);

            if (state.installed)
            {
                spdlog::info("Mod compatibility: {} is installed{}; its settings take precedence where both mods change the same thing.",
                    mod.name, haveSettings ? "" : " (no settings file found)");
            }
            else if (mgs4e::log::Verbose())
            {
                spdlog::info("Mod compatibility: {} is not installed.", mod.name);
            }
        }

        // Manifests: the mods that describe themselves. Our own settings file name is skipped
        // so a manifest written for it cannot make us yield to ourselves.
        g_Manifests = mgs4e::manifests::Scan(mgs4e::game::Root(), std::string(MGS4E_NAME) + ".settings");
        for (const auto& m : g_Manifests)
        {
            int on = 0;
            for (const auto& c : m.claims) { on += c.active ? 1 : 0; }
            if (m.asiPresent || mgs4e::log::Verbose())
            {
                spdlog::info("Mod compatibility: {} ({}) {}; {} feature claim(s), {} on.",
                    m.name, m.manifest.filename().string(), m.asiPresent ? "is installed" : "is not installed", m.claims.size(), on);
            }
        }
    }

    void LogOverlaps()
    {
        // Feature -> the claims on it that are on, across manifest mods.
        std::vector<std::pair<std::string, std::vector<std::pair<const mgs4e::manifests::Mod*, const mgs4e::manifests::Claim*>>>> byFeature;
        for (const auto& m : g_Manifests)
        {
            for (const auto& c : m.claims)
            {
                if (!c.active) { continue; }
                auto it = std::find_if(byFeature.begin(), byFeature.end(), [&](const auto& f) { return f.first == c.feature; });
                if (it == byFeature.end())
                {
                    byFeature.push_back({ c.feature, {} });
                    it = byFeature.end() - 1;
                }
                it->second.push_back({ &m, &c });
            }
        }
        for (const auto& [feature, owners] : byFeature)
        {
            if (owners.size() < 2) { continue; }
            std::string list;
            for (const auto& [m, c] : owners)
            {
                if (!list.empty()) { list += " and "; }
                list += m->name + " (" + c->key + " = " + (c->alwaysOn ? std::string("always on") : (c->value.empty() ? std::string("default") : c->value)) + ")";
            }
            spdlog::warn("Mod overlap: {} is turned on in both {}; their patches will stack at start-up. Open the {} tool and choose one - it turns the others off.",
                mgs4e::features::DisplayName(feature), list, MGS4E_DISPLAY_NAME);
        }
    }

    bool Installed(std::size_t mod)
    {
        return mod < kModCount && g_Mods[mod].installed;
    }

    std::string Value(std::size_t mod, const char* theirSection, const char* theirKey)
    {
        if (mod >= kModCount)
        {
            return std::string();
        }
        return g_Mods[mod].settings.Raw(theirSection, theirKey).value_or(std::string());
    }

    bool Yields(const char* section, const char* key)
    {
        if (const Entry* entry = Find(section, key))
        {
            const std::string theirs = Value(entry->mod, entry->theirSection, entry->theirKey);
            if (TheirsWins(*entry, Installed(entry->mod), theirs))
            {
                spdlog::info("{}: set by {} ({} = {}); {} setting ignored.",
                    key, kMods[entry->mod].name, entry->theirKey, theirs.empty() ? "default" : theirs, MGS4E_DISPLAY_NAME);
                return true;
            }
        }
        if (const mgs4e::features::Ours* ours = mgs4e::features::OurClaim(section, key))
        {
            for (const auto& m : g_Manifests)
            {
                for (const auto& c : m.claims)
                {
                    if (c.active && c.feature == ours->feature)
                    {
                        spdlog::info("{}: {} has {} on ({} = {}); {} setting ignored.",
                            key, m.name, mgs4e::features::DisplayName(c.feature), c.key,
                            c.alwaysOn ? std::string("always on") : (c.value.empty() ? std::string("default") : c.value), MGS4E_DISPLAY_NAME);
                        return true;
                    }
                }
            }
        }
        return false;
    }
}
