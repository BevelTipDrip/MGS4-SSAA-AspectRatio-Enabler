#pragma once

#include "fields.hpp"

#include "compat_table.hpp"
#include "features.hpp"
#include "manifest_claims.hpp"

#include <map>
#include <span>
#include <string>
#include <string_view>

// The games the tool serves. One window serves one game: the tool is dropped into a game's
// install folder and detects which game from the marker beside it (paths.hpp). Everything
// that used to be a compile-time MGS4 constant - names, the Steam app id, the field table,
// the feature claims, the compat table, where manifests are looked for, the prose on the
// Troubleshooting and About pages - is a member here, so the MGS4 window is the one it always
// was and the Peace Walker window is the same code over its own descriptor.
namespace mgs4e::tool
{
    using SettingsMap = std::map<std::string, std::map<std::string, std::string>>;

    struct Game
    {
        const char* id;                 // "MGS4" / "MGSPW": the --game argument
        const char* name;               // <name>.settings, <name>.asi, logs\<name>_Game.log
        const char* displayName;        // window title, dialogs, our name in feature ownership
        const char* gameTitle;          // the game as the user knows it
        const char* repoUrl;
        const wchar_t* steamAppId;      // steam://rungameid/<id>
        const wchar_t* exeDir;          // the folder under the install root holding the exe
        const wchar_t* exeName;         // the exe: the install marker is exeDir\exeName

        const std::vector<Page>& (*pages)();
        const std::vector<const Field*>& (*allFields)();
        const Field* (*findField)(const std::string& section, const std::string& key);

        std::span<const mgs4e::compat::Mod> mods;
        std::span<const mgs4e::compat::Entry> table;
        std::span<const mgs4e::features::Ours> ours;
        std::string_view manifestSuffix;
        std::span<const mgs4e::manifests::Place> places;

        // Settings-file hooks. migrateLoaded rewrites what was read before validation (legacy
        // spellings, retired keys); importExtras copies undocumented keys from another mod's
        // file and returns how many. Either may be null.
        void (*migrateLoaded)(SettingsMap& values);
        int (*importExtras)(const mgs4e::Ini& other, SettingsMap& values);

        const char* troubleshootingText;   // %1 = name, %2 = repo url (wxString::Format)
        const char* aboutSubtitle;
        const char* aboutBody;             // %1 = name, %2 = name
        const char* loaderNote;            // %1 = mod name, %2 = files (a mod tab whose copy is not where the loader looks)
        const char* notFoundHint;          // how the install folder is recognised, for the not-found message
        const char* licenseNote;

        const mgs4e::compat::Entry* Find(const char* section, const char* key) const;
        const mgs4e::features::Ours* OurClaim(std::string_view section, std::string_view key) const;
    };

    const Game& Mgs4();
    const Game& PeaceWalker();
    std::span<const Game* const> AllGames();

    // The Peace Walker field table (tool/src/pw/fields.cpp); MGS4's is fields.hpp's.
    namespace pw
    {
        const std::vector<Page>& Pages();
        const std::vector<const Field*>& AllFields();
        const Field* FindField(const std::string& section, const std::string& key);
    }
}
