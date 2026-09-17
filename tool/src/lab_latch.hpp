#pragma once

#include "games.hpp"

#include <filesystem>
#include <string>

// Latched lab mode. A Lab build of a plugin reads <name>.lab.settings on a boot where the harness
// has dropped the one-boot marker <name>.lab; that marker is consumed each time. The latch is the
// persistent form: <name>.lab.latched, written and removed only by this tool, honoured by a Lab
// plugin on every boot and ignored by a Release plugin entirely. While latched, this tool edits the
// lab settings file too, so the game and the tool agree on one file.
namespace mgs4e::tool::lablatch
{
    struct State
    {
        bool labPluginDeployed = false;   // <exeDir>\scripts\<name>.asi is a Lab build
        bool labSettingsExist = false;    // <root>\<name>.lab.settings is present
        bool latched = false;             // <root>\<name>.lab.latched is present
        std::filesystem::path plugin;     // where the deployed plugin was looked for
    };

    // Reads the deployed plugin without running it: a Lab build carries the literal the packager
    // refuses on, a Release build does not.
    State Inspect(const Game& game, const std::filesystem::path& gameRoot);

    std::filesystem::path LatchFile(const Game& game, const std::filesystem::path& gameRoot);
    std::filesystem::path LabSettingsFile(const Game& game, const std::filesystem::path& gameRoot);

    // Creates the lab settings from the shipped settings if they do not exist (never the reverse),
    // then writes the latch. Returns an error description, or empty on success.
    std::string Latch(const Game& game, const std::filesystem::path& gameRoot);

    // Removes the latch only; the lab settings and their knobs are kept.
    std::string Revert(const Game& game, const std::filesystem::path& gameRoot);
}
