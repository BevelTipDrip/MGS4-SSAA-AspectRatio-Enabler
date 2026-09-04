#include "pch.hpp"
#include "upstream.hpp"

#include "game.hpp"
#include "log.hpp"
#include "ini.hpp"
#include "overlap_table.hpp"
#include "version.hpp"

namespace pfc::upstream
{
    namespace
    {
        bool g_Loaded = false;
        pfc::Ini g_Settings;
    }

    void Detect()
    {
        // The module handle is the authoritative answer: it is what actually patched the game.
        // The on-disk check only covers a loader that has not got to it yet, which does not
        // happen with the alphabetical order, but costs nothing to allow for.
        std::wstring asiName = std::wstring(PFC_NEIGHBOUR_ASI, PFC_NEIGHBOUR_ASI + std::strlen(PFC_NEIGHBOUR_ASI));
        g_Loaded = GetModuleHandleW(asiName.c_str()) != nullptr;
        if (!g_Loaded)
        {
            std::error_code ec;
            const std::filesystem::path scripts = pfc::game::ExePath().parent_path() / "scripts" / PFC_NEIGHBOUR_ASI;
            const std::filesystem::path beside = pfc::game::ExePath().parent_path() / PFC_NEIGHBOUR_ASI;
            g_Loaded = std::filesystem::exists(scripts, ec) || std::filesystem::exists(beside, ec);
        }

        const std::filesystem::path settings = pfc::game::Root() / PFC_NEIGHBOUR_SETTINGS_FILE;
        const bool haveSettings = g_Settings.Load(settings);

        if (g_Loaded)
        {
            spdlog::info("{} is installed{}; its settings take precedence where both mods patch the same thing.",
                PFC_NEIGHBOUR_NAME, haveSettings ? "" : " (no settings file found)");
        }
        else if (pfc::log::Verbose())
        {
            spdlog::info("{} is not installed.", PFC_NEIGHBOUR_NAME);
        }
    }

    bool Loaded()
    {
        return g_Loaded;
    }

    std::string Value(const char* theirSection, const char* theirKey)
    {
        return g_Settings.Raw(theirSection, theirKey).value_or(std::string());
    }

    bool Yields(const char* section, const char* key)
    {
        const pfc::overlap::Entry* entry = pfc::overlap::Find(section, key);
        if (!entry)
        {
            return false;
        }
        const std::string theirs = Value(entry->theirSection, entry->theirKey);
        if (!pfc::overlap::TheirsWins(*entry, g_Loaded, theirs))
        {
            return false;
        }
        spdlog::info("{}: set by {} ({} = {}); {} setting ignored.",
            key, PFC_NEIGHBOUR_NAME, entry->theirKey, theirs.empty() ? "default" : theirs, PFC_DISPLAY_NAME);
        return true;
    }
}
