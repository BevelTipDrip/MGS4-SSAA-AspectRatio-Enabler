#include "pch.hpp"
#include "paths.hpp"

namespace pfc::tool
{
    std::optional<std::filesystem::path> FindGameRoot()
    {
        std::error_code ec;
        const std::filesystem::path exe = wxStandardPaths::Get().GetExecutablePath().ToStdWstring();

        for (const std::filesystem::path candidate : { std::filesystem::current_path(ec), exe.parent_path() })
        {
            if (candidate.empty())
            {
                continue;
            }
            if (std::filesystem::exists(candidate / "MGS4" / "mgs4.exe", ec))
            {
                return std::filesystem::canonical(candidate, ec);
            }
        }
        return std::nullopt;
    }
}
