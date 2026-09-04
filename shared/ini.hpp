#pragma once

#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

// A small read-only INI reader.
//
// Understands "[Section]" headers, "key = value" lines, ";" and "#" comments, a UTF-8 BOM,
// and values wrapped in double quotes (the quotes are stripped). Section and key lookups
// are case-sensitive; whitespace around keys and values is trimmed. Duplicate keys keep the
// last value. Shared by the ASI and the settings tool, and used for the neighbour's settings
// file as well as our own - it never writes.
namespace pfc
{
    class Ini
    {
    public:
        // Loads a file. Returns false if it cannot be opened; the object is then empty.
        bool Load(const std::filesystem::path& file)
        {
            m_Sections.clear();
            m_Loaded = false;

            std::ifstream in(file, std::ios::binary);
            if (!in)
            {
                return false;
            }
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            Parse(content);
            m_Loaded = true;
            return true;
        }

        void Parse(std::string_view content)
        {
            if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF
                && static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF)
            {
                content.remove_prefix(3);
            }

            std::string section;
            size_t pos = 0;
            while (pos <= content.size())
            {
                const size_t eol = content.find('\n', pos);
                std::string_view line = content.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
                pos = (eol == std::string_view::npos) ? content.size() + 1 : eol + 1;

                line = Trim(line);
                if (line.empty() || line[0] == ';' || line[0] == '#')
                {
                    continue;
                }
                if (line.front() == '[')
                {
                    const size_t close = line.find(']');
                    section = std::string(Trim(line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1)));
                    continue;
                }
                const size_t eq = line.find('=');
                if (eq == std::string_view::npos)
                {
                    continue;
                }
                const std::string key(Trim(line.substr(0, eq)));
                std::string_view value = Trim(line.substr(eq + 1));
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                {
                    value = value.substr(1, value.size() - 2);
                }
                m_Sections[section][key] = std::string(value);
            }
        }

        bool Loaded() const { return m_Loaded; }

        bool HasSection(std::string_view section) const
        {
            return m_Sections.find(std::string(section)) != m_Sections.end();
        }

        bool Has(std::string_view section, std::string_view key) const
        {
            return Raw(section, key).has_value();
        }

        // The value as written (quotes stripped), or nothing when the key is absent.
        std::optional<std::string> Raw(std::string_view section, std::string_view key) const
        {
            const auto s = m_Sections.find(std::string(section));
            if (s == m_Sections.end())
            {
                return std::nullopt;
            }
            const auto k = s->second.find(std::string(key));
            if (k == s->second.end())
            {
                return std::nullopt;
            }
            return k->second;
        }

        // Typed reads. Absent -> nullopt. Present but unparsable -> nullopt as well; callers
        // that want to log a malformed value can check Has() first.
        template<typename T>
        std::optional<T> Get(std::string_view section, std::string_view key) const
        {
            const auto raw = Raw(section, key);
            if (!raw)
            {
                return std::nullopt;
            }
            return Convert<T>(*raw);
        }

        template<typename T>
        static std::optional<T> Convert(std::string_view text)
        {
            text = Trim(text);
            if constexpr (std::is_same_v<T, std::string>)
            {
                return std::string(text);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                std::string lower(text);
                for (char& c : lower)
                {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (lower == "1" || lower == "true" || lower == "yes" || lower == "on")
                {
                    return true;
                }
                if (lower == "0" || lower == "false" || lower == "no" || lower == "off")
                {
                    return false;
                }
                return std::nullopt;
            }
            else if constexpr (std::is_integral_v<T>)
            {
                T value {};
                const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
                if (result.ec != std::errc {} || result.ptr != text.data() + text.size())
                {
                    return std::nullopt;
                }
                return value;
            }
            else
            {
                // Floating point: from_chars for floats is fine on MSVC.
                T value {};
                const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
                if (result.ec != std::errc {} || result.ptr != text.data() + text.size())
                {
                    return std::nullopt;
                }
                return value;
            }
        }

        static std::string_view Trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
            {
                s.remove_prefix(1);
            }
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
            {
                s.remove_suffix(1);
            }
            return s;
        }

        const std::map<std::string, std::map<std::string, std::string>>& Sections() const { return m_Sections; }

    private:
        std::map<std::string, std::map<std::string, std::string>> m_Sections;
        bool m_Loaded = false;
    };
}
