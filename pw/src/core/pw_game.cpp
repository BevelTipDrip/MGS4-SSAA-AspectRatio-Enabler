#include "pch.hpp"
#include "pw_game.hpp"

#include "game.hpp"
#include "version.hpp"

namespace mgspwe::game
{
    bool IsPeaceWalker()
    {
        std::wstring name = mgs4e::game::ExePath().filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
        const std::string narrow = MGSPWE_EXE_NAME;   // ASCII, so the widening is a plain copy
        std::wstring wanted(narrow.begin(), narrow.end());
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), ::towlower);
        return name == wanted;
    }
}
