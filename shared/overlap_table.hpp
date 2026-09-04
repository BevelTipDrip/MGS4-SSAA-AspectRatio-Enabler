#pragma once

#include <string_view>

// Settings PF Companion and MGSPatriotFix both implement.
//
// Both mods patch the game in the same process, and where they patch the same thing the
// results would stack or fight. MGSPatriotFix loads first and its setting takes precedence:
// the ASI skips its own patch, and the settings tool greys the field out and shows the value
// MGSPatriotFix is using. Shared by both binaries so they agree on what counts as an overlap.
//
// Section and key names on the MGSPatriotFix side are theirs and must match their settings
// file exactly; ours are the PFCompanion.settings names.
namespace pfc::overlap
{
    enum class Rule
    {
        // Their patch is installed whenever their ASI is loaded, whatever the value.
        WhenLoaded,
        // Their patch is installed only when their value is non-zero.
        WhenNonZero,
    };

    struct Entry
    {
        const char* ourSection;
        const char* ourKey;
        const char* theirSection;
        const char* theirKey;
        Rule rule;
    };

    inline constexpr Entry kTable[] = {
        // Their mid hooks rewrite MaxAnisotropy on every 8x sampler description unconditionally
        // (their value is clamped to 1..16, so "off" does not exist); ours would only raise it
        // further. One owner is clearer.
        { "Graphics", "Anisotropic Filtering", "Enhancements && Tweaks", "Anisotropic Filtering Level", Rule::WhenLoaded },
        // Their detour writes an absolute shadow buffer size after the parser returns, which
        // lands after our scaled value and overwrites it. With their value at 0 the detour is
        // not installed and our scale applies.
        { "Graphics", "Shadow Resolution Scale (%)", "Enhancements && Tweaks", "Custom Shadow Resolution", Rule::WhenNonZero },
    };

    // The table entry for one of our keys, or nullptr when the key has no overlap.
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

    // Whether the neighbour's setting takes precedence, given whether its ASI is present and
    // the raw value from its settings file (empty when the key is absent).
    inline bool TheirsWins(const Entry& entry, bool theirAsiPresent, std::string_view theirValue)
    {
        if (!theirAsiPresent)
        {
            return false;
        }
        switch (entry.rule)
        {
        case Rule::WhenLoaded:
            return true;
        case Rule::WhenNonZero:
        {
            // Accepts "0", "0.0", " 0 " and an absent key as zero.
            for (char c : theirValue)
            {
                if (c >= '1' && c <= '9')
                {
                    return true;
                }
            }
            return false;
        }
        }
        return false;
    }
}
