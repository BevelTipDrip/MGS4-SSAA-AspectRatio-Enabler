#pragma once

#include "games.hpp"

#include <filesystem>
#include <optional>
#include <utility>

namespace mgs4e::tool
{
    // A game's install folder: the one holding <exeDir>\<exeName>. The working directory is
    // tried first (Steam and shortcuts set it), then the folder the tool runs from.
    std::optional<std::filesystem::path> FindGameRoot(const Game& game);

    // The game the tool sits in: each known game in turn, the first whose marker is beside the
    // tool. With `preferredId` set (--game), only that game is tried.
    struct Install
    {
        const Game* game;
        std::filesystem::path root;
    };
    std::optional<Install> FindInstalledGame(const std::string& preferredId);
}
