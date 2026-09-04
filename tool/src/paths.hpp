#pragma once

#include <filesystem>
#include <optional>

namespace pfc::tool
{
    // The game's install folder: the one holding MGS4\mgs4.exe. The working directory is
    // tried first (Steam and shortcuts set it), then the folder the tool runs from.
    std::optional<std::filesystem::path> FindGameRoot();
}
