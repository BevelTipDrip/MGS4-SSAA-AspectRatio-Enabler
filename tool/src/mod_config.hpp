#pragma once

#include "fields.hpp"

#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Other mods' config files, edited on their own tabs.
//
// Some mods that share the game with this one keep a small .ini next to their .asi and have
// no tool of their own. When such a file is found in the game folder the tool grows a tab for
// it. Only the listed keys are edited, and a save rewrites just those values in place -
// comments, order and anything else in the file are left as they were. This is separate from
// compat_table.hpp, which is about settings the two mods both control; these are settings
// only the other mod has.
//
// Two sources tell the tool what to expose:
//
//   1. A manifest the mod ships beside its own .ini, named <anything>.MGS4Enabler.ini. The
//      mod's author writes it, so they control exactly what the tab shows; nothing in this
//      tool needs to change. The format is documented in docs/mod-tabs.md and in
//      LoadManifest below.
//   2. The built-in table, Known(), for mods that never ship a manifest. A manifest for the
//      same .ini wins over the table entry.
namespace mgs4e::tool::modconfig
{
    struct Key
    {
        Field field;         // section, key, type, default, range, choices, help
        const char* label;   // what the row is called on the tab
        // What a bool is written as. Most small inis use 1/0; a manifest may say otherwise.
        const char* trueText = "1";
        const char* falseText = "0";
        // The feature this key gates (shared/features.hpp), the value at which the mod's
        // patch is off, and whether it is on regardless of the value while the mod is there.
        // Empty feature: the key claims nothing.
        const char* feature = "";
        const char* offText = "";
        bool alwaysOn = false;
    };

    struct Mod
    {
        const char* name;       // as the tab is titled
        const char* fileName;   // the .ini, looked for beside the .asi (or the manifest)
        const char* asiFile;    // the mod's .asi; may be empty for a manifest that names none
        const char* url;        // where the mod comes from; may be empty
        const char* blurb;      // one or two sentences on the tab, above the settings; may be empty
        std::vector<Key> keys;

        // Manifest-loaded mods only: where the description came from, anything wrong with it
        // (shown on the tab so the author sees it), and the storage behind the const char*
        // members above. Table entries leave these empty and point at string literals.
        std::filesystem::path manifest;
        std::vector<std::string> problems;
        std::shared_ptr<std::deque<std::string>> strings;
    };

    const std::vector<Mod>& Known();

    // The manifest file name suffix, matched without regard to case: MyMod.MGS4Enabler.ini.
    constexpr const char* kManifestSuffix = ".MGS4Enabler.ini";

    // Reads one manifest. Returns nothing only when the file cannot be read or has no [Mod]
    // block at all; a manifest with mistakes still yields a Mod, with the mistakes listed in
    // `problems` and the bad keys left out, so the author gets told rather than ignored.
    std::shared_ptr<Mod> LoadManifest(const std::filesystem::path& manifest);

    // A config file that was found. `loadable` says whether the game will actually load the
    // mod from where it sits: mgs4.exe lives in MGS4\ and its ASI loader scans MGS4\ and the
    // plugins, scripts and update folders under it, so a copy in the install folder's own
    // scripts\ folder is found by this tool but never runs.
    struct Found
    {
        std::shared_ptr<const Mod> mod;
        std::filesystem::path file;
        std::filesystem::path asi;   // empty when only the .ini is there
        bool loadable = false;
    };

    // One entry per mod found - manifests first, then table entries whose file exists and
    // has no manifest - preferring a loadable copy of each.
    //
    // For a manifest: the ini is looked for beside the manifest, then in the game root (some
    // mods keep their settings there, beside the launcher, while the .asi sits in
    // MGS4\scripts). `loadable` is judged by the .asi the manifest names - present in any
    // folder the game's loader scans - and is true when it names none, since there is then
    // nothing to judge. Table entries keep the old rule: ini and asi beside each other.
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
