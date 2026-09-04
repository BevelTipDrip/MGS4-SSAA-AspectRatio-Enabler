#pragma once

#include <filesystem>

// The process PF Companion is loaded into.
//
// Only mgs4.exe is supported. The exe lives in <root>\MGS4\, and everything PF Companion
// reads or writes on disk - the settings file, the lab marker, the log - is relative to
// <root>, the folder Steam installs the game into.
namespace pfc::game
{
    // Resolves the exe path and its parents. Called once, first thing in initialisation;
    // safe to call again.
    void Detect();

    // The main executable's module handle. Every pattern scan and RVA is relative to it.
    HMODULE Module();

    // Full path of the running exe.
    const std::filesystem::path& ExePath();

    // The game's install root: the parent of the folder holding the exe.
    const std::filesystem::path& Root();

    // Whether the running process is mgs4.exe. Nothing is patched otherwise.
    bool IsMgs4();
}
