#include "pch.hpp"
#include "compat.hpp"

#include "game.hpp"
#include "log.hpp"
#include "ini.hpp"
#include "compat_table.hpp"
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
        const Entry* entry = Find(section, key);
        if (!entry)
        {
            return false;
        }
        const std::string theirs = Value(entry->mod, entry->theirSection, entry->theirKey);
        if (!TheirsWins(*entry, Installed(entry->mod), theirs))
        {
            return false;
        }
        spdlog::info("{}: set by {} ({} = {}); {} setting ignored.",
            key, kMods[entry->mod].name, entry->theirKey, theirs.empty() ? "default" : theirs, MGS4E_DISPLAY_NAME);
        return true;
    }
}
