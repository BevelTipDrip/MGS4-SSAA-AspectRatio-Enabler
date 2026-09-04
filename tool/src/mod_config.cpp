#include "pch.hpp"
#include "mod_config.hpp"

#include "ini.hpp"

namespace mgs4e::tool::modconfig
{
    const std::vector<Mod>& Known()
    {
        using F = Field;
        static const std::vector<Mod> mods = {
            {
                "MGSFPSUnlock",
                "MGSFPSUnlock.ini",
                "MGSFPSUnlock.asi",
                "https://github.com/cipherxof/MGSFPSUnlock",
                "MGSFPSUnlock (cipherxof) lets the game run above or below its 60 fps cap. It has no settings "
                "tool of its own, so its one setting is here. Only that value is written; the rest of its "
                "file is left alone.",
                {
                    { F::Int("Settings", "TargetFrameRate",
                        "The frame rate the game is unlocked to. 60 is the game's own limit; MGSFPSUnlock's default "
                        "when its file is missing is 60 as well.\n\n"
                        "Anything above 60 depends on the mod's patches, which its author describes as a work in "
                        "progress, so the odd thing may misbehave at high rates. Your monitor's refresh rate is the "
                        "usual choice.",
                        60, 15, 1000),
                      "Target frame rate" },
                },
            },
        };
        return mods;
    }

    std::vector<Found> Detect(const std::filesystem::path& gameRoot)
    {
        // Where the mod's own loader logic looks, relative to mgs4.exe (MGS4\): the exe's folder
        // and its plugins, scripts and update subfolders. The same names under the install
        // folder are checked too, because that is where a zip extracted one level too high
        // lands; those copies are reported but flagged as not loadable.
        struct Place { const char* dir; bool loadable; };
        static const Place places[] = {
            { "MGS4", true }, { "MGS4\\scripts", true }, { "MGS4\\plugins", true }, { "MGS4\\update", true },
            { ".", false }, { "scripts", false }, { "plugins", false }, { "update", false },
        };

        std::vector<Found> found;
        std::error_code ec;
        for (const Mod& mod : Known())
        {
            std::optional<Found> best;
            for (const Place& place : places)
            {
                const std::filesystem::path dir = gameRoot / place.dir;
                const std::filesystem::path file = dir / mod.fileName;
                if (!std::filesystem::exists(file, ec))
                {
                    continue;
                }
                Found f;
                f.mod = &mod;
                f.file = file;
                f.loadable = place.loadable;
                if (std::filesystem::exists(dir / mod.asiFile, ec))
                {
                    f.asi = dir / mod.asiFile;
                }
                // Places are in preference order, so the first loadable hit wins outright and
                // a non-loadable one only stands in until a loadable one turns up.
                if (!best || (!best->loadable && f.loadable))
                {
                    best = f;
                }
                if (best->loadable)
                {
                    break;
                }
            }
            if (best)
            {
                found.push_back(*best);
            }
        }
        return found;
    }

    // --- File ------------------------------------------------------------------------------

    bool File::Load(const std::filesystem::path& path)
    {
        m_Path = path;
        m_Lines.clear();
        m_Values.clear();
        m_Loaded.clear();

        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            return false;
        }
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        m_Eol = content.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        size_t pos = 0;
        while (pos < content.size())
        {
            size_t eol = content.find('\n', pos);
            const size_t end = eol == std::string::npos ? content.size() : eol;
            std::string line = content.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            m_Lines.push_back(std::move(line));
            pos = eol == std::string::npos ? content.size() : eol + 1;
        }

        mgs4e::Ini ini;
        ini.Parse(content);
        m_Values = ini.Sections();
        m_Loaded = m_Values;
        return true;
    }

    std::string File::Get(const std::string& section, const std::string& key) const
    {
        const auto s = m_Values.find(section);
        if (s == m_Values.end())
        {
            return {};
        }
        const auto k = s->second.find(key);
        return k == s->second.end() ? std::string() : k->second;
    }

    void File::Set(const std::string& section, const std::string& key, const std::string& value)
    {
        m_Values[section][key] = value;
    }

    namespace
    {
        std::string_view Trim(std::string_view s)
        {
            return mgs4e::Ini::Trim(s);
        }

        // "[Settings]" -> "Settings"; anything else -> nothing.
        std::optional<std::string> SectionOf(const std::string& line)
        {
            const std::string_view t = Trim(line);
            if (t.empty() || t.front() != '[')
            {
                return std::nullopt;
            }
            const size_t close = t.find(']');
            return std::string(Trim(t.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1)));
        }

        // The key of a "key = value" line, or nothing for comments, blanks and headers.
        std::optional<std::string> KeyOf(const std::string& line)
        {
            const std::string_view t = Trim(line);
            if (t.empty() || t.front() == ';' || t.front() == '#' || t.front() == '[')
            {
                return std::nullopt;
            }
            const size_t eq = t.find('=');
            if (eq == std::string_view::npos)
            {
                return std::nullopt;
            }
            return std::string(Trim(t.substr(0, eq)));
        }
    }

    std::optional<std::string> File::Save()
    {
        // Put each changed value back on the line that holds its key, keeping everything up
        // to and including the "=" (and the space after it, if there was one) as written. A
        // key with no line yet goes at the end of its section; a section with no header yet
        // goes at the end of the file.
        for (const auto& [section, keys] : m_Values)
        {
            for (const auto& [key, value] : keys)
            {
                const auto loaded = m_Loaded.find(section);
                if (loaded != m_Loaded.end())
                {
                    const auto k = loaded->second.find(key);
                    if (k != loaded->second.end() && k->second == value)
                    {
                        continue;
                    }
                }

                // Find the section's line range.
                size_t sectionStart = m_Lines.size();
                size_t sectionEnd = m_Lines.size();
                bool inSection = section.empty();
                if (inSection)
                {
                    sectionStart = 0;
                }
                for (size_t i = 0; i < m_Lines.size(); ++i)
                {
                    if (const auto s = SectionOf(m_Lines[i]))
                    {
                        if (inSection)
                        {
                            sectionEnd = i;
                            break;
                        }
                        if (*s == section)
                        {
                            inSection = true;
                            sectionStart = i + 1;
                            sectionEnd = m_Lines.size();
                        }
                    }
                }

                if (!inSection)
                {
                    if (!m_Lines.empty() && !Trim(m_Lines.back()).empty())
                    {
                        m_Lines.push_back("");
                    }
                    m_Lines.push_back("[" + section + "]");
                    m_Lines.push_back(key + " = " + value);
                    continue;
                }

                bool replaced = false;
                for (size_t i = sectionStart; i < sectionEnd; ++i)
                {
                    const auto k = KeyOf(m_Lines[i]);
                    if (!k || *k != key)
                    {
                        continue;
                    }
                    const std::string& line = m_Lines[i];
                    size_t cut = line.find('=') + 1;
                    if (cut < line.size() && line[cut] == ' ')
                    {
                        ++cut;
                    }
                    m_Lines[i] = line.substr(0, cut) + value;
                    replaced = true;
                    break;
                }
                if (!replaced)
                {
                    // After the section's last key line, before any trailing blank lines.
                    size_t at = sectionEnd;
                    while (at > sectionStart && Trim(m_Lines[at - 1]).empty())
                    {
                        --at;
                    }
                    m_Lines.insert(m_Lines.begin() + static_cast<std::ptrdiff_t>(at), key + " = " + value);
                }
            }
        }

        std::string out;
        for (const std::string& line : m_Lines)
        {
            out += line;
            out += m_Eol;
        }
        std::ofstream file(m_Path, std::ios::binary | std::ios::trunc);
        if (!file || !(file << out))
        {
            return "Could not write " + m_Path.string() + ". Is it read-only, or open in another program?";
        }
        m_Loaded = m_Values;
        return std::nullopt;
    }
}
