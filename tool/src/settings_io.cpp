#include "pch.hpp"
#include "settings_io.hpp"

#include "fields.hpp"
#include "ini.hpp"
#include "version.hpp"

namespace mgs4e::tool
{
    namespace
    {
        // Bools are written as true/false whatever spelling was read; ints and choices as-is.
        std::string Canonical(const Field& field, const std::string& text)
        {
            if (field.type == Field::Type::Bool)
            {
                const auto b = mgs4e::Ini::Convert<bool>(text);
                return (b && *b) ? "true" : "false";
            }
            return std::string(mgs4e::Ini::Trim(text));
        }

        // Choice values and anything with spaces are quoted so the line reads unambiguously.
        std::string Quoted(const Field* field, const std::string& value)
        {
            const bool quote = field ? field->type == Field::Type::Choice
                                     : value.find(' ') != std::string::npos;
            return quote ? "\"" + value + "\"" : value;
        }
    }

    std::filesystem::path Settings::FileFor(const Game& game, const std::filesystem::path& gameRoot)
    {
        return gameRoot / (std::string(game.name) + ".settings");
    }

    bool Settings::Load(const std::filesystem::path& file)
    {
        m_Values.clear();
        mgs4e::Ini ini;
        const bool exists = ini.Load(file);
        m_Values = ini.Sections();

        if (m_Game->migrateLoaded)
        {
            m_Game->migrateLoaded(m_Values);
        }

        ValidateKnown();
        m_Loaded = m_Values;
        return exists;
    }

    void Settings::ValidateKnown()
    {
        for (const Field* f : m_Game->allFields())
        {
            auto& section = m_Values[f->section];
            const auto it = section.find(f->key);
            if (it == section.end() || !f->Accepts(std::string(mgs4e::Ini::Trim(it->second))))
            {
                section[f->key] = f->DefaultText();
            }
            else
            {
                it->second = Canonical(*f, it->second);
            }
        }
    }

    void Settings::ResetToDefaults()
    {
        for (const Field* f : m_Game->allFields())
        {
            m_Values[f->section][f->key] = f->DefaultText();
        }
    }

    int Settings::ImportFrom(const std::filesystem::path& otherFile)
    {
        mgs4e::Ini other;
        if (!other.Load(otherFile))
        {
            return 0;
        }

        int imported = 0;
        for (const Field* f : m_Game->allFields())
        {
            const auto raw = other.Raw(f->section, f->key);
            if (!raw)
            {
                continue;
            }
            const std::string text(mgs4e::Ini::Trim(*raw));
            if (!f->Accepts(text))
            {
                continue;
            }
            m_Values[f->section][f->key] = Canonical(*f, text);
            ++imported;
        }
        if (m_Game->importExtras)
        {
            imported += m_Game->importExtras(other, m_Values);
        }
        return imported;
    }

    std::optional<std::string> Settings::Save(const std::filesystem::path& file) const
    {
        std::string out;
        out += "; " + std::string(m_Game->displayName) + " " MGS4E_VERSION_STRING " settings. Edit with \"" + m_Game->displayName + ".exe\" or by hand;\n";
        out += "; unknown keys are kept. Read by " + std::string(m_Game->name) + ".asi when the game starts.\n";

        // Known sections first, in table order, each with its known keys in table order and
        // then whatever else the section held.
        std::vector<std::string> written;
        auto writeSection = [&](const std::string& name, const std::vector<const Field*>& fields)
        {
            const auto sec = m_Values.find(name);
            out += "\n[" + name + "]\n";
            std::map<std::string, bool> done;
            for (const Field* f : fields)
            {
                const std::string value = sec != m_Values.end() && sec->second.contains(f->key) ? sec->second.at(f->key) : f->DefaultText();
                out += std::string(f->key) + " = " + Quoted(f, value) + "\n";
                done[f->key] = true;
            }
            if (sec != m_Values.end())
            {
                for (const auto& [key, value] : sec->second)
                {
                    if (!done.contains(key))
                    {
                        out += key + " = " + Quoted(nullptr, value) + "\n";
                    }
                }
            }
            written.push_back(name);
        };

        std::vector<std::string> order;
        std::map<std::string, std::vector<const Field*>> bySection;
        for (const Field* f : m_Game->allFields())
        {
            if (!bySection.contains(f->section))
            {
                order.push_back(f->section);
            }
            bySection[f->section].push_back(f);
        }
        for (const std::string& name : order)
        {
            writeSection(name, bySection[name]);
        }
        for (const auto& [name, keys] : m_Values)
        {
            if (std::find(written.begin(), written.end(), name) == written.end() && !keys.empty())
            {
                writeSection(name, {});
            }
        }

        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            return "Could not write " + file.string() + ". Check that the folder is not read-only.";
        }
        f << out;
        if (!f)
        {
            return "Writing " + file.string() + " failed part way through.";
        }
        return std::nullopt;
    }

    std::string Settings::Get(const std::string& section, const std::string& key) const
    {
        const auto s = m_Values.find(section);
        if (s == m_Values.end())
        {
            return {};
        }
        const auto k = s->second.find(key);
        return k == s->second.end() ? std::string() : k->second;
    }

    void Settings::Set(const std::string& section, const std::string& key, const std::string& value)
    {
        m_Values[section][key] = value;
    }
}
