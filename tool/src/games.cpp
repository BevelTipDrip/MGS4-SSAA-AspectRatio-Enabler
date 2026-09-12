#include "pch.hpp"
#include "games.hpp"

#include "ini.hpp"
#include "settings_keys.hpp"
#include "version.hpp"
#include "pw/compat_table.hpp"
#include "pw/features.hpp"
#include "pw/identity.hpp"
#include "pw/manifest_places.hpp"

namespace mgs4e::tool
{
    namespace
    {
        // --- MGS4: the strings the tool showed before it served two games, moved here verbatim.

        // The setting's first shape was a plain on/off; carry its meaning into the choice.
        // And the sample-count key withdrawn in 0.0.6 is not carried through a save.
        void Mgs4Migrate(SettingsMap& values)
        {
            auto& graphics = values[mgs4e::keys::Graphics];
            if (!graphics.contains(mgs4e::keys::UltrawideHud))
            {
                const auto legacy = graphics.find(mgs4e::keys::UltrawideHudFix);
                if (legacy != graphics.end())
                {
                    const auto on = mgs4e::Ini::Convert<bool>(legacy->second);
                    graphics[mgs4e::keys::UltrawideHud] = (on && *on) ? mgs4e::keys::UltrawideHud_Expanded : mgs4e::keys::UltrawideHud_Stretched;
                    graphics.erase(legacy);
                }
            }
            graphics.erase(mgs4e::keys::ShadowSampleCount_Retired);
        }

        // From another mod's settings file: the legacy on/off (only when the choice itself is
        // not there) and the two undocumented graphics keys, so a tuned setup keeps working.
        int Mgs4ImportExtras(const mgs4e::Ini& other, SettingsMap& values)
        {
            int imported = 0;
            if (!other.Has(mgs4e::keys::Graphics, mgs4e::keys::UltrawideHud))
            {
                if (const auto on = other.Get<bool>(mgs4e::keys::Graphics, mgs4e::keys::UltrawideHudFix))
                {
                    values[mgs4e::keys::Graphics][mgs4e::keys::UltrawideHud] = *on ? mgs4e::keys::UltrawideHud_Expanded : mgs4e::keys::UltrawideHud_Stretched;
                    ++imported;
                }
            }
            for (const char* key : { mgs4e::keys::FixReticleTruncation, mgs4e::keys::MaxInternalBufferWidth })
            {
                if (const auto raw = other.Raw(mgs4e::keys::Graphics, key))
                {
                    values[mgs4e::keys::Graphics][key] = std::string(mgs4e::Ini::Trim(*raw));
                    ++imported;
                }
            }
            return imported;
        }

        const Game kMgs4 = {
            "MGS4",
            MGS4E_NAME,
            MGS4E_DISPLAY_NAME,
            "METAL GEAR SOLID 4",
            MGS4E_REPO_URL,
            L"2492670",
            L"MGS4",
            L"mgs4.exe",
            &Pages, &AllFields, &FindField,
            std::span<const mgs4e::compat::Mod>(mgs4e::compat::kMods),
            std::span<const mgs4e::compat::Entry>(mgs4e::compat::kTable),
            std::span<const mgs4e::features::Ours>(mgs4e::features::kOurs),
            ".MGS4Enabler.ini",
            std::span<const mgs4e::manifests::Place>(mgs4e::manifests::kPlaces),
            &Mgs4Migrate,
            &Mgs4ImportExtras,
            "To capture a log for a bug report: tick Debug Logging, save, launch the game from Steam and "
            "reproduce the problem, then quit and attach logs\\%s_Game.log from the game's install folder. "
            "The log is rewritten on every launch, so copy it before starting the game again.\n"
            "\n"
            "For a HUD element in the wrong place on an ultrawide or 4:3 screen, press F11 in-game while it is on "
            "screen; a record of every HUD element being drawn at that moment goes into the log. Take a screenshot "
            "at the same time (Steam's F12) and attach both.\n"
            "\n"
            "Report problems at %s.",
            "Graphics settings for METAL GEAR SOLID 4 (Master Collection)",
            "%s.asi patches the game as it starts: internal resolution scaling, sharper shadows, anisotropic "
            "filtering, FXAA control, and an undistorted HUD on ultrawide and 4:3 displays. This tool writes the "
            "settings it reads.\n"
            "\n"
            "It shares the game with other mods. Where another mod it knows about changes the same thing as one of "
            "these settings, that mod's setting takes precedence and this tool says so. MGSPatriotFix is recognised "
            "today; support for more mods will be added.\n"
            "\n"
            "Nothing here phones home. There is no updater and no telemetry. The files touched are %s.settings, "
            "the log, and another mod's config file when you change it on that mod's tab.",
            "This copy of %s is not where the game loads mods from, so it is not running. mgs4.exe is in the "
            "MGS4 folder and its loader only picks up .asi files in MGS4 and MGS4\\scripts. Move %s "
            "into MGS4\\scripts; MGS4\\winmm.dll is already the loader, and a wininet.dll in the install "
            "folder does nothing there.",
            "the one that contains the MGS4 folder with mgs4.exe in it",
            MGS4E_LICENSE_NOTE,
        };

        const Game kPeaceWalker = {
            "MGSPW",
            MGSPWE_NAME,
            MGSPWE_DISPLAY_NAME,
            MGSPWE_GAME_TITLE,
            MGSPWE_REPO_URL,
            L"2492660",
            L"mgspw",
            L"METAL GEAR SOLID PEACE WALKER.exe",
            &pw::Pages, &pw::AllFields, &pw::FindField,
            std::span<const mgs4e::compat::Mod>(mgspwe::compat::kMods),
            std::span<const mgs4e::compat::Entry>(mgspwe::compat::kTable),
            std::span<const mgs4e::features::Ours>(mgspwe::features::kOurs),
            mgspwe::manifests::kSuffix,
            std::span<const mgs4e::manifests::Place>(mgspwe::manifests::kPlaces),
            nullptr,
            nullptr,
            "To capture a log for a bug report: tick Debug Logging, save, launch the game from Steam and "
            "reproduce the problem, then quit and attach logs\\%s_Game.log from the game's install folder. "
            "The log is rewritten on every launch, so copy it before starting the game again.\n"
            "\n"
            "Report problems at %s.",
            "Graphics settings for METAL GEAR SOLID: Peace Walker (Master Collection)",
            "%s.asi is the Peace Walker build of the Enabler. It is at the research stage: it loads, reads its "
            "settings and logs, and patches nothing yet. This tool writes the settings it reads.\n"
            "\n"
            "It shares the game with other mods through the same manifest mechanism as the MGS4 build: a mod that "
            "ships a <Mod>.MGSPWEnabler.ini beside its .ini gets a tab here.\n"
            "\n"
            "Nothing here phones home. There is no updater and no telemetry. The files touched are %s.settings, "
            "the log, and another mod's config file when you change it on that mod's tab.",
            "This copy of %s is not where the game loads mods from, so it is not running. The game's exe is in the "
            "mgspw folder and its loader only picks up .asi files in mgspw and mgspw\\scripts. Move %s "
            "into mgspw\\scripts; mgspw\\winmm.dll is the loader.",
            "the one that contains the mgspw folder with METAL GEAR SOLID PEACE WALKER.exe in it",
            MGSPWE_LICENSE_NOTE,
        };

        const Game* const kAll[] = { &kMgs4, &kPeaceWalker };
    }

    const mgs4e::compat::Entry* Game::Find(const char* section, const char* key) const
    {
        for (const mgs4e::compat::Entry& e : table)
        {
            if (std::string_view(e.ourSection) == section && std::string_view(e.ourKey) == key)
            {
                return &e;
            }
        }
        return nullptr;
    }

    const mgs4e::features::Ours* Game::OurClaim(std::string_view section, std::string_view key) const
    {
        for (const mgs4e::features::Ours& o : ours)
        {
            if (section == o.section && key == o.key)
            {
                return &o;
            }
        }
        return nullptr;
    }

    const Game& Mgs4() { return kMgs4; }
    const Game& PeaceWalker() { return kPeaceWalker; }
    std::span<const Game* const> AllGames() { return kAll; }
}
