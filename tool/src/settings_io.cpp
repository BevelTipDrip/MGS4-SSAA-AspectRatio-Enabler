#include "pch.hpp"
#include "settings_io.hpp"

#include "fields.hpp"
#include "ini.hpp"
#include "settings_keys.hpp"
#include "version.hpp"

namespace pfc::tool
{
    namespace
    {
        // Bools are written as true/false whatever spelling was read; ints and choices as-is.
        std::string Canonical(const Field& field, const std::string& text)
        {
            if (field.type == Field::Type::Bool)
            {
                const auto b = pfc::Ini::Convert<bool>(text);
                return (b && *b) ? "true" : "false";
            }
            return std::string(pfc::Ini::Trim(text));
        }

        // Choice values and anything with spaces are quoted so the line reads unambiguously.
        std::string Quoted(const Field* field, const std::string& value)
        {
            const bool quote = field ? field->type == Field::Type::Choice
                                     : value.find(' ') != std::string::npos;
            return quote ? "\"" + value + "\"" : value;
        }
    }

    std::filesystem::path Settings::FileFor(const std::filesystem::path& gameRoot)
    {
        return gameRoot / (PFC_NAME ".settings");
    }

    bool Settings::Load(const std::filesystem::path& file)
    {
        m_Values.clear();
        pfc::Ini ini;
        const bool exists = ini.Load(file);
        m_Values = ini.Sections();

        // The setting's first shape was a plain on/off; carry its meaning into the choice.
        auto& graphics = m_Values[pfc::keys::Graphics];
        if (!graphics.contains(pfc::keys::UltrawideHud))
        {
            const auto legacy = graphics.find(pfc::keys::UltrawideHudFix);
            if (legacy != graphics.end())
            {
                const auto on = pfc::Ini::Convert<bool>(legacy->second);
                graphics[pfc::keys::UltrawideHud] = (on && *on) ? pfc::keys::UltrawideHud_Expanded : pfc::keys::UltrawideHud_Stretched;
                graphics.erase(legacy);
            }
        }

        ValidateKnown();
        m_Loaded = m_Values;
        return exists;
    }

    void Settings::ValidateKnown()
    {
        for (const Field* f : AllFields())
        {
            auto& section = m_Values[f->section];
            const auto it = section.find(f->key);
            if (it == section.end() || !f->Accepts(std::string(pfc::Ini::Trim(it->second))))
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
        for (const Field* f : AllFields())
        {
            m_Values[f->section][f->key] = f->DefaultText();
        }
    }

    int Settings::ImportFrom(const std::filesystem::path& otherFile)
    {
        pfc::Ini other;
        if (!other.Load(otherFile))
        {
            return 0;
        }

        int imported = 0;

        // The legacy on/off, only when the choice itself is not there.
        if (!other.Has(pfc::keys::Graphics, pfc::keys::UltrawideHud))
        {
            if (const auto on = other.Get<bool>(pfc::keys::Graphics, pfc::keys::UltrawideHudFix))
            {
                m_Values[pfc::keys::Graphics][pfc::keys::UltrawideHud] = *on ? pfc::keys::UltrawideHud_Expanded : pfc::keys::UltrawideHud_Stretched;
                ++imported;
            }
        }

        for (const Field* f : AllFields())
        {
            const auto raw = other.Raw(f->section, f->key);
            if (!raw)
            {
                continue;
            }
            const std::string text(pfc::Ini::Trim(*raw));
            if (!f->Accepts(text))
            {
                continue;
            }
            m_Values[f->section][f->key] = Canonical(*f, text);
            ++imported;
        }

        // The two undocumented graphics keys travel too, so a tuned setup keeps working.
        for (const char* key : { pfc::keys::FixReticleTruncation, pfc::keys::MaxInternalBufferWidth })
        {
            if (const auto raw = other.Raw(pfc::keys::Graphics, key))
            {
                m_Values[pfc::keys::Graphics][key] = std::string(pfc::Ini::Trim(*raw));
                ++imported;
            }
        }
        return imported;
    }

    std::optional<std::string> Settings::Save(const std::filesystem::path& file) const
    {
        std::string out;
        out += "; " PFC_DISPLAY_NAME " " PFC_VERSION_STRING " settings. Edit with \"" PFC_DISPLAY_NAME ".exe\" or by hand;\n";
        out += "; unknown keys are kept. Read by " PFC_NAME ".asi when the game starts.\n";

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
        for (const Field* f : AllFields())
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
