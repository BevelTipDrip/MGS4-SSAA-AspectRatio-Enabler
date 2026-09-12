#pragma once

#include "games.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

// <name>.settings for one game: the values the tool edits plus anything else found in the file.
//
// Keys the tool knows (the game's field table) are validated against their field on load and
// reset to the default when malformed. Keys it does not know (the undocumented ones, lab
// keys, a user's own notes in unknown sections) are carried through a save untouched, so hand
// edits survive.
namespace mgs4e::tool
{
    class Settings
    {
    public:
        explicit Settings(const Game& game) : m_Game(&game) {}

        // Where the settings live for a given game root.
        static std::filesystem::path FileFor(const Game& game, const std::filesystem::path& gameRoot);

        // Loads the file. Returns false when it does not exist (values are then the defaults).
        bool Load(const std::filesystem::path& file);

        // Sets every known field to its default; unknown keys are kept.
        void ResetToDefaults();

        // Pulls our keys out of another mod's settings file (earlier builds of these graphics
        // settings shipped inside another mod and wrote them into its file). Only keys the tool knows are
        // taken; the other file is never modified. Returns how many values were imported.
        int ImportFrom(const std::filesystem::path& otherFile);

        // Writes the file. Returns an error message on failure, nothing on success.
        std::optional<std::string> Save(const std::filesystem::path& file) const;

        std::string Get(const std::string& section, const std::string& key) const;
        void Set(const std::string& section, const std::string& key, const std::string& value);

        // Values that differ from what was loaded (or from the defaults on a fresh file).
        bool Dirty() const { return m_Values != m_Loaded; }

    private:
        void ValidateKnown();

        const Game* m_Game;
        SettingsMap m_Values;
        SettingsMap m_Loaded;
    };
}
