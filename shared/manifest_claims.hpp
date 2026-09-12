#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "features.hpp"
#include "ini.hpp"

// The feature claims other mods make in their manifests (<Mod>.MGS4Enabler.ini, see
// docs/mod-tabs.md), read the same way by the ASI and the settings tool.
//
// A manifest block [Section/Key] with `Feature = <id>` says: this key gates the mod's patch
// for that feature; it is off at `Off = <value>` (a bool's false spelling when absent), or
// always on while the mod's .asi is present when `AlwaysOn = true`. The scan finds every
// manifest in the folders the game's loader reads, the mod's ini (beside the manifest or in
// the install folder) and its .asi, and reads each claimed key's current value.
namespace mgs4e::manifests
{
    struct Claim
    {
        std::string feature;
        std::string section;    // in the mod's ini; empty for a file without headers
        std::string key;
        std::string label;      // the manifest's Label, else the key
        std::string off;        // the value at which the patch is off
        bool alwaysOn = false;
        std::string value;      // as found in the mod's ini now (empty: absent)
        bool active = false;    // the patch is in force: asi present and (alwaysOn or value not off)
    };

    struct Mod
    {
        std::string name;
        std::filesystem::path manifest;
        std::filesystem::path ini;   // may not exist
        std::filesystem::path asi;   // empty when none found (or none named)
        bool asiNamed = false;
        bool asiPresent = false;
        std::vector<Claim> claims;
    };

    namespace detail
    {
        inline std::string Lower(std::string s)
        {
            for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
            return s;
        }

        inline bool EndsWithNoCase(const std::string& name, std::string_view suffix)
        {
            const std::string l = Lower(name), s = Lower(std::string(suffix));
            return l.size() >= s.size() && l.compare(l.size() - s.size(), s.size(), s) == 0;
        }

        inline bool IsTrue(std::string_view v)
        {
            const std::string l = Lower(std::string(mgs4e::Ini::Trim(v)));
            return l == "true" || l == "1" || l == "yes" || l == "on";
        }

        // Section headers in the order written; the parser's map would sort them.
        inline std::vector<std::string> BlockOrder(const std::string& content)
        {
            std::vector<std::string> order;
            size_t pos = 0;
            while (pos < content.size())
            {
                size_t eol = content.find('\n', pos);
                const size_t end = eol == std::string::npos ? content.size() : eol;
                std::string_view line = mgs4e::Ini::Trim(std::string_view(content).substr(pos, end - pos));
                pos = eol == std::string::npos ? content.size() : eol + 1;
                if (line.empty() || line.front() != '[') { continue; }
                const size_t close = line.find(']');
                std::string name(mgs4e::Ini::Trim(line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1)));
                if (name != "Mod" && std::find(order.begin(), order.end(), name) == order.end())
                {
                    order.push_back(std::move(name));
                }
            }
            return order;
        }
    }

    inline constexpr const char* kSuffix = ".MGS4Enabler.ini";

    // Where the game's loader reads .asi files from, relative to the install folder, and the
    // same names one level up (a zip extracted too high): manifests are looked for in all of
    // them, .asi presence only counts in the first four.
    struct Place { const char* dir; bool loadable; };
    inline constexpr Place kPlaces[] = {
        { "MGS4", true }, { "MGS4\\scripts", true }, { "MGS4\\plugins", true }, { "MGS4\\update", true },
        { ".", false }, { "scripts", false }, { "plugins", false }, { "update", false },
    };

    // Reads one manifest's claims. Returns false when it cannot be read or has no [Mod].
    // `skipIni`: a manifest whose Ini is this file name is ignored (our own settings file,
    // should anyone write a manifest for it).
    inline bool Load(const std::filesystem::path& manifest, const std::filesystem::path& dir,
                     const std::filesystem::path& gameRoot, std::string_view skipIni,
                     std::string_view suffix, std::span<const Place> places, Mod& out)
    {
        std::error_code ec;
        std::ifstream in(manifest, std::ios::binary);
        if (!in) { return false; }
        const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        mgs4e::Ini ini;
        ini.Parse(content);
        if (!ini.HasSection("Mod")) { return false; }

        out = Mod{};
        out.manifest = manifest;
        const std::string fallback = manifest.filename().string();
        out.name = ini.Raw("Mod", "Name").value_or(fallback.substr(0, fallback.size() - suffix.size()));
        const std::string iniName = ini.Raw("Mod", "Ini").value_or(std::string());
        if (iniName.empty() || iniName.find('\\') != std::string::npos || iniName.find('/') != std::string::npos)
        {
            return false;
        }
        if (detail::Lower(iniName) == detail::Lower(std::string(skipIni)))
        {
            return false;
        }
        out.ini = dir / iniName;
        if (!std::filesystem::exists(out.ini, ec) && std::filesystem::exists(gameRoot / iniName, ec))
        {
            out.ini = gameRoot / iniName;
        }
        const std::string asiName = ini.Raw("Mod", "Asi").value_or(std::string());
        out.asiNamed = !asiName.empty();
        if (out.asiNamed)
        {
            for (const Place& place : places)
            {
                const std::filesystem::path asi = gameRoot / place.dir / asiName;
                if (std::filesystem::exists(asi, ec))
                {
                    out.asi = asi;
                    if (place.loadable) { out.asiPresent = true; break; }
                }
            }
        }
        else
        {
            out.asiPresent = std::filesystem::exists(out.ini, ec);   // nothing else to judge by
        }

        mgs4e::Ini values;
        values.Load(out.ini);

        for (const std::string& block : detail::BlockOrder(content))
        {
            const std::string feature = ini.Raw(block, "Feature").value_or(std::string());
            if (feature.empty()) { continue; }
            Claim c;
            c.feature = std::string(mgs4e::Ini::Trim(feature));
            const size_t slash = block.find('/');
            c.section = slash == std::string::npos ? std::string() : std::string(mgs4e::Ini::Trim(block.substr(0, slash)));
            c.key = std::string(mgs4e::Ini::Trim(slash == std::string::npos ? std::string_view(block) : std::string_view(block).substr(slash + 1)));
            if (c.key.empty()) { continue; }
            c.label = ini.Raw(block, "Label").value_or(c.key);
            c.alwaysOn = detail::IsTrue(ini.Raw(block, "AlwaysOn").value_or(std::string()));
            c.off = ini.Raw(block, "Off").value_or(std::string());
            if (c.off.empty())
            {
                // A bool is off at its false spelling; anything else has to say.
                const std::string type = detail::Lower(ini.Raw(block, "Type").value_or(std::string()));
                if (type == "bool")
                {
                    const std::string spelling = ini.Raw(block, "BoolText").value_or("1|0");
                    const size_t bar = spelling.find('|');
                    c.off = bar == std::string::npos ? "0" : std::string(mgs4e::Ini::Trim(std::string_view(spelling).substr(bar + 1)));
                }
                else if (!c.alwaysOn)
                {
                    continue;   // no way to tell on from off: not a claim
                }
            }
            c.value = values.Raw(c.section, c.key).value_or(std::string());
            c.active = out.asiPresent && (c.alwaysOn || !features::IsOff(c.value, c.off));
            out.claims.push_back(std::move(c));
        }
        return true;
    }

    // Every manifest under the game root, one entry per ini name (a loadable copy preferred).
    inline std::vector<Mod> Scan(const std::filesystem::path& gameRoot, std::string_view skipIni,
                                 std::string_view suffix, std::span<const Place> places)
    {
        std::vector<Mod> mods;
        std::error_code ec;
        for (const Place& place : places)
        {
            const std::filesystem::path dir = gameRoot / place.dir;
            if (!std::filesystem::is_directory(dir, ec)) { continue; }
            std::vector<std::filesystem::path> files;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
            {
                if (entry.is_regular_file(ec) && detail::EndsWithNoCase(entry.path().filename().string(), suffix))
                {
                    files.push_back(entry.path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const std::filesystem::path& file : files)
            {
                Mod mod;
                if (!Load(file, dir, gameRoot, skipIni, suffix, places, mod)) { continue; }
                const std::string id = detail::Lower(mod.ini.filename().string());
                auto seen = std::find_if(mods.begin(), mods.end(), [&](const Mod& m) { return detail::Lower(m.ini.filename().string()) == id; });
                if (seen == mods.end())
                {
                    mods.push_back(std::move(mod));
                }
            }
        }
        return mods;
    }

    // The MGS4 build's spellings: its own suffix and loader folders. The Peace Walker build
    // passes its own (shared/pw/manifest_places.hpp); the scan itself is the same code.
    inline bool Load(const std::filesystem::path& manifest, const std::filesystem::path& dir,
                     const std::filesystem::path& gameRoot, std::string_view skipIni, Mod& out)
    {
        return Load(manifest, dir, gameRoot, skipIni, kSuffix, kPlaces, out);
    }

    inline std::vector<Mod> Scan(const std::filesystem::path& gameRoot, std::string_view skipIni)
    {
        return Scan(gameRoot, skipIni, kSuffix, kPlaces);
    }
}
