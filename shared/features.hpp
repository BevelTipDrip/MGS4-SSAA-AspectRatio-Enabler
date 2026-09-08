#pragma once

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

#include "settings_keys.hpp"

// Features: the shared names for things more than one mod may patch.
//
// Several MGS4 mods change the same few things - shadow resolution, dynamic resolution,
// anisotropic filtering - each gated on a value in its own config file. When two of them are
// on at once their patches stack or fight at start-up, in whatever order the loader ran them.
// A feature id is the common name a mod gives such a setting, in its manifest (`Feature =`
// in a [Section/Key] block, see docs/mod-tabs.md) or, for this mod, in kOurs below. With
// the ids shared, the settings tool can see every claim on a feature and keep exactly one
// on, and the ASI can stand down where another mod already owns one.
//
// Ids are free-form strings compared exactly; the table only supplies display names for the
// known ones. A new id in a manifest works as well as an old one.
namespace mgs4e::features
{
    struct Known
    {
        const char* id;
        const char* name;   // as shown to the user
    };

    inline constexpr Known kKnown[] = {
        { "shadow-resolution",    "Shadow resolution" },
        { "shadow-filter",        "Shadow filtering" },
        { "dynamic-resolution",   "Dynamic resolution" },
        { "anisotropic-filtering", "Anisotropic filtering" },
        { "internal-resolution",  "Internal render resolution" },
        { "frame-rate",           "Frame rate" },
        { "skip-intro",           "Skipping the intro" },
        { "motion-blur",          "Motion blur" },
        { "api",                  "Renderer (DirectX version)" },
        { "window-mode",          "Window mode" },
    };

    inline std::string_view DisplayName(std::string_view id)
    {
        for (const Known& k : kKnown)
        {
            if (id == k.id)
            {
                return k.name;
            }
        }
        return id;
    }

    // This mod's own claims: the setting, the feature it patches, and the value at which it
    // does nothing (so another mod may own the feature). Shared so the ASI and the tool agree.
    struct Ours
    {
        const char* section;
        const char* key;
        const char* feature;
        const char* off;
    };

    inline constexpr Ours kOurs[] = {
        { keys::Graphics, keys::InternalResolutionScale, "internal-resolution",   "100" },
        { keys::Graphics, keys::ShadowResolutionScale,   "shadow-resolution",     "100" },
        { keys::Graphics, keys::AnisotropicFiltering,    "anisotropic-filtering", "0" },
        { keys::Graphics, keys::DirectXVersion,          "api",                   keys::DirectXVersion_Off },
        { keys::Graphics, keys::WindowMode,              "window-mode",           keys::WindowMode_Off },
    };

    inline const Ours* OurClaim(std::string_view section, std::string_view key)
    {
        for (const Ours& o : kOurs)
        {
            if (section == o.section && key == o.key)
            {
                return &o;
            }
        }
        return nullptr;
    }

    // Whether a config value counts as "off" against a declared off value: exact text, or
    // both parse as the same number (so "0", "0.0" and "0.000" agree, as do "1.0" and "1").
    inline bool IsOff(std::string_view value, std::string_view off)
    {
        auto trim = [](std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.remove_prefix(1); }
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) { s.remove_suffix(1); }
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') { s = s.substr(1, s.size() - 2); }
            return s;
        };
        value = trim(value);
        off = trim(off);
        if (value.empty())
        {
            return true;   // an absent key is the mod's default, which is off for every mod we know
        }
        if (value == off)
        {
            return true;
        }
        auto number = [](std::string_view s, double& out)
        {
            if (s.empty()) { return false; }
            char* end = nullptr;
            const std::string copy(s);
            out = std::strtod(copy.c_str(), &end);
            return end && *end == '\0';
        };
        double a = 0.0, b = 0.0;
        if (number(value, a) && number(off, b))
        {
            return a == b;
        }
        // Bool spellings.
        auto lower = [](std::string_view s)
        {
            std::string l(s);
            for (char& c : l) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
            return l;
        };
        const std::string lv = lower(value), lo = lower(off);
        auto boolOf = [](const std::string& s, bool& out)
        {
            if (s == "true" || s == "yes" || s == "on" || s == "1") { out = true; return true; }
            if (s == "false" || s == "no" || s == "off" || s == "0") { out = false; return true; }
            return false;
        };
        bool bv = false, bo = false;
        if (boolOf(lv, bv) && boolOf(lo, bo))
        {
            return bv == bo;
        }
        return false;
    }
}
