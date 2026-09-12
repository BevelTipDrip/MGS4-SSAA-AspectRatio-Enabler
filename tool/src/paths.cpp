#include "pch.hpp"
#include "paths.hpp"

namespace mgs4e::tool
{
    std::optional<std::filesystem::path> FindGameRoot(const Game& game)
    {
        std::error_code ec;
        const std::filesystem::path exe = wxStandardPaths::Get().GetExecutablePath().ToStdWstring();

        for (const std::filesystem::path candidate : { std::filesystem::current_path(ec), exe.parent_path() })
        {
            if (candidate.empty())
            {
                continue;
            }
            if (std::filesystem::exists(candidate / game.exeDir / game.exeName, ec))
            {
                return std::filesystem::canonical(candidate, ec);
            }
        }
        return std::nullopt;
    }

    std::optional<Install> FindInstalledGame(const std::string& preferredId)
    {
        for (const Game* game : AllGames())
        {
            if (!preferredId.empty() && preferredId != game->id)
            {
                continue;
            }
            if (const auto root = FindGameRoot(*game))
            {
                return Install { game, *root };
            }
        }
        return std::nullopt;
    }
}
