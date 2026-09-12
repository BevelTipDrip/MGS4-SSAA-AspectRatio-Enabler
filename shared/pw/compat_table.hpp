#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "../compat_table.hpp"

// Mod compatibility for the Peace Walker build: the types and the precedence rule are the
// MGS4 build's (../compat_table.hpp); the data is this game's. No mod is listed yet - other
// Peace Walker mods are expected to describe themselves with a manifest
// (<Mod>.MGSPWEnabler.ini, see docs/mod-tabs.md), which needs no entry here.
namespace mgspwe::compat
{
    using mgs4e::compat::Mod;
    using mgs4e::compat::Rule;
    using mgs4e::compat::Entry;
    using mgs4e::compat::TheirsWins;

    inline constexpr std::array<Mod, 0> kMods {};
    inline constexpr std::size_t kModCount = kMods.size();
    inline constexpr std::array<Entry, 0> kTable {};

    inline const Entry* Find(const char* ourSection, const char* ourKey)
    {
        for (const Entry& e : kTable)
        {
            if (std::string_view(e.ourSection) == ourSection && std::string_view(e.ourKey) == ourKey)
            {
                return &e;
            }
        }
        return nullptr;
    }
}
