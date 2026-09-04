#pragma once

#include "fields.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Other mods' config files, edited on their own tabs.
//
// Some mods that share the game with this one keep a small .ini next to their .asi and have
// no tool of their own. Each entry in Known() names such a file and the keys worth exposing;
// when the file is found in the game folder the tool grows a tab for it. Only the listed keys
// are edited, and a save rewrites just those values in place - comments, order and anything
// else in the file are left as they were. This is separate from compat_table.hpp, which is
// about settings the two mods both control; these are settings only the other mod has.
namespace mgs4e::tool::modconfig
{
    struct Key
    {
        Field field;         // section, key, type, default, range, choices, help
        const char* label;   // what the row is called on the tab
    };

    struct Mod
    {
        const char* name;       // as the tab is titled
        const char* fileName;   // the .ini, looked for beside the .asi
        const char* asiFile;    // the mod's .asi
        const char* url;        // where the mod comes from
        const char* blurb;      // one or two sentences on the tab, above the settings
        std::vector<Key> keys;
    };

    const std::vector<Mod>& Known();

    // A config file that was found. `loadable` says whether the game will actually load the
    // mod from where it sits: mgs4.exe lives in MGS4\ and its ASI loader scans MGS4\ and the
    // plugins, scripts and update folders under it, so a copy in the install folder's own
    // scripts\ folder is found by this tool but never runs.
    struct Found
    {
        const Mod* mod = nullptr;
        std::filesystem::path file;
        std::filesystem::path asi;   // empty when only the .ini is there
        bool loadable = false;
    };

    // One entry per known mod whose config file exists, preferring a loadable copy.
    std::vector<Found> Detect(const std::filesystem::path& gameRoot);

    // An editable copy of one file. Values are read with the shared INI parser; Save() puts
    // the changed values back into the original lines and writes nothing else.
    class File
    {
    public:
        bool Load(const std::filesystem::path& path);

        std::string Get(const std::string& section, const std::string& key) const;
        void Set(const std::string& section, const std::string& key, const std::string& value);

        bool Dirty() const { return m_Values != m_Loaded; }

        // Returns an error message on failure, nothing on success.
        std::optional<std::string> Save();

        const std::filesystem::path& Path() const { return m_Path; }

    private:
        using Map = std::map<std::string, std::map<std::string, std::string>>;

        std::filesystem::path m_Path;
        std::vector<std::string> m_Lines;   // the file as read, without line endings
        std::string m_Eol = "\r\n";
        Map m_Values;
        Map m_Loaded;
    };
}
