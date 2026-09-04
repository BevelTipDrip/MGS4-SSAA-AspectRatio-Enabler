#pragma once

#include <cstddef>
#include <string_view>

// Mod compatibility.
//
// Other mods patch the same game in the same process, and where one of them changes the same
// thing as one of our settings the results would stack or fight. This table lists the mods
// we know how to share the game with and, for each, the settings that collide. When a listed
// mod is installed and its setting is in force, ours stands down: the ASI skips its own patch
// and the settings tool greys the field out and shows the value that mod is using. Shared by
// both binaries so they agree on what counts as a collision.
//
// Adding a mod is a new `Mod` entry plus its `Entry` rows. Section and key names on the other
// mod's side are theirs and must match their settings file exactly; ours are the
// MGS4Enabler.settings names.
namespace mgs4e::compat
{
    // A mod we know how to share the game with.
    struct Mod
    {
        const char* name;          // as shown in the log and the tool
        const char* asiFile;       // file name of its ASI, looked for in MGS4\scripts and beside mgs4.exe
        const char* settingsFile;  // its settings file, relative to the game root; read only
    };

    inline constexpr Mod kMods[] = {
        { "MGSPatriotFix", "MGSPatriotFix.asi", "MGSPatriotFix.settings" },
    };

    inline constexpr std::size_t kModCount = sizeof(kMods) / sizeof(kMods[0]);

    enum class Rule
    {
        // Their patch is installed whenever their ASI is loaded, whatever the value.
        WhenLoaded,
        // Their patch is installed only when their value is non-zero.
        WhenNonZero,
    };

    struct Entry
    {
        std::size_t mod;           // index into kMods
        const char* ourSection;
        const char* ourKey;
        const char* theirSection;
        const char* theirKey;
        Rule rule;
    };

    inline constexpr Entry kTable[] = {
        // MGSPatriotFix's mid hooks rewrite MaxAnisotropy on every 8x sampler description
        // unconditionally (its value is clamped to 1..16, so "off" does not exist); ours would
        // only raise it further. One owner is clearer.
        { 0, "Graphics", "Anisotropic Filtering", "Enhancements && Tweaks", "Anisotropic Filtering Level", Rule::WhenLoaded },
        // Its detour writes an absolute shadow buffer size after the parser returns, which
        // lands after our scaled value and overwrites it. With its value at 0 the detour is
        // not installed and our scale applies.
        { 0, "Graphics", "Shadow Resolution Scale (%)", "Enhancements && Tweaks", "Custom Shadow Resolution", Rule::WhenNonZero },
    };

    // The table entry for one of our keys, or nullptr when no known mod collides with it.
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

    // Whether the other mod's setting takes precedence, given whether its ASI is present and
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
