#include "pch.hpp"
#include "mod_config.hpp"

#include "ini.hpp"

#include <cctype>

namespace mgs4e::tool::modconfig
{
    const std::vector<Mod>& Known()
    {
        // Empty since 0.0.6: every mod the tool knows ships as a manifest instead (the
        // Enabler's own zip carries manifests\*.MGS4Enabler.ini into MGS4\scripts), so a mod
        // author never needs a change here. The table stays as the fallback it was built as.
        static const std::vector<Mod> mods = {};
        return mods;
    }

    namespace
    {
        std::string_view Trim(std::string_view s)
        {
            return mgs4e::Ini::Trim(s);
        }

        std::string Lower(std::string s)
        {
            for (char& c : s)
            {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
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

        std::vector<std::string> SplitLines(const std::string& content)
        {
            std::vector<std::string> lines;
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
                lines.push_back(std::move(line));
                pos = eol == std::string::npos ? content.size() : eol + 1;
            }
            return lines;
        }

        // A manifest string with "\n" written in it becomes a real line break: help text and
        // the blurb are the only multi-line things and INI files have no other way to say so.
        std::string Unescape(std::string s)
        {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n')
                {
                    out += '\n';
                    ++i;
                }
                else
                {
                    out += s[i];
                }
            }
            return out;
        }

        std::vector<std::string> SplitBar(const std::string& s)
        {
            std::vector<std::string> parts;
            size_t pos = 0;
            while (true)
            {
                const size_t bar = s.find('|', pos);
                std::string part(Trim(s.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos)));
                if (!part.empty())
                {
                    parts.push_back(std::move(part));
                }
                if (bar == std::string::npos)
                {
                    break;
                }
                pos = bar + 1;
            }
            return parts;
        }

        // Every const char* a Mod hands out has to outlive it; the manifest's strings live in
        // the Mod's own deque, which never moves what it holds.
        const char* Keep(Mod& mod, std::string s)
        {
            mod.strings->push_back(std::move(s));
            return mod.strings->back().c_str();
        }

        bool EndsWithNoCase(const std::string& name, const char* suffix)
        {
            const std::string lower = Lower(name);
            const std::string suf = Lower(suffix);
            return lower.size() >= suf.size() && lower.compare(lower.size() - suf.size(), suf.size(), suf) == 0;
        }
    }

    // A manifest is an INI file:
    //
    //   [Mod]
    //   Name = MyMod                      ; the tab's title (required)
    //   Ini = MyMod.ini                   ; the file the tab edits, beside the manifest (required)
    //   Asi = MyMod.asi                   ; the mod's .asi, for the "not where the game loads
    //                                     ;   from" check (optional)
    //   Url = https://...                 ; a link on the tab (optional)
    //   Blurb = What the mod does.        ; text above the settings; "\n" breaks a line (optional)
    //
    //   [Settings/TargetFrameRate]        ; one block per row: [Section/Key] of the mod's ini;
    //   Label = Target frame rate         ;   "[Key]" alone for an ini with no section headers
    //   Type = int                        ; int, bool or choice (required)
    //   Default = 60                      ; what the row shows when the key is missing
    //   Min = 15                          ; int: both required
    //   Max = 1000
    //   Choices = Fast|Balanced|Quality   ; choice: required, "|" between the values as written
    //   BoolText = true|false             ; bool: what to write; 1|0 when absent
    //   Help = What it does.              ; the tooltip; "\n" breaks a line
    //   Feature = shadow-resolution       ; what the key switches on (shared/features.hpp);
    //   Off = 0                           ;   the value at which it is off (a bool: its false
    //   AlwaysOn = true                   ;   spelling); or on whenever the mod is installed
    //
    // Only keys with a block are ever written. Mistakes are collected in `problems` and shown
    // on the tab, and the row concerned is left out; the tab itself still appears.
    std::shared_ptr<Mod> LoadManifest(const std::filesystem::path& manifest)
    {
        std::ifstream in(manifest, std::ios::binary);
        if (!in)
        {
            return nullptr;
        }
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

        mgs4e::Ini ini;
        ini.Parse(content);
        if (!ini.HasSection("Mod"))
        {
            return nullptr;
        }

        auto mod = std::make_shared<Mod>();
        mod->strings = std::make_shared<std::deque<std::string>>();
        mod->manifest = manifest;
        mod->name = "";
        mod->fileName = "";
        mod->asiFile = "";
        mod->url = "";
        mod->blurb = "";

        const auto text = [&](std::string_view section, std::string_view key) -> std::string
        {
            return ini.Raw(section, key).value_or(std::string());
        };

        const std::string name = text("Mod", "Name");
        const std::string iniName = text("Mod", "Ini");
        const std::string fallback = manifest.filename().string();
        mod->name = Keep(*mod, name.empty() ? fallback.substr(0, fallback.size() - std::string(kManifestSuffix).size()) : name);
        if (name.empty())
        {
            mod->problems.push_back("[Mod] has no Name; the tab is titled after the manifest instead.");
        }
        if (iniName.empty())
        {
            mod->problems.push_back("[Mod] has no Ini, so there is no file to edit. Nothing can be saved from this tab.");
        }
        else if (iniName.find('\\') != std::string::npos || iniName.find('/') != std::string::npos)
        {
            mod->problems.push_back("[Mod] Ini must be a bare file name beside the manifest, not a path.");
        }
        else
        {
            mod->fileName = Keep(*mod, iniName);
        }
        mod->asiFile = Keep(*mod, text("Mod", "Asi"));
        mod->url = Keep(*mod, text("Mod", "Url"));
        mod->blurb = Keep(*mod, Unescape(text("Mod", "Blurb")));

        // Blocks in the order written; the parser's map would sort them.
        std::vector<std::string> order;
        for (const std::string& line : SplitLines(content))
        {
            if (const auto s = SectionOf(line))
            {
                if (*s != "Mod" && std::find(order.begin(), order.end(), *s) == order.end())
                {
                    order.push_back(*s);
                }
            }
        }

        for (const std::string& block : order)
        {
            const size_t slash = block.find('/');
            const std::string section = slash == std::string::npos ? std::string() : std::string(Trim(block.substr(0, slash)));
            const std::string key(Trim(slash == std::string::npos ? std::string_view(block) : std::string_view(block).substr(slash + 1)));
            const std::string where = "[" + block + "]";
            if (key.empty())
            {
                mod->problems.push_back(where + ": no key name after the slash.");
                continue;
            }

            const std::string type = Lower(text(block, "Type"));
            const std::string label = text(block, "Label");
            const std::string def = text(block, "Default");
            const std::string help = Unescape(text(block, "Help"));
            const char* sectionText = Keep(*mod, section);
            const char* keyText = Keep(*mod, key);
            const char* helpText = Keep(*mod, help);

            Key row;
            row.label = Keep(*mod, label.empty() ? key : label);
            const std::string feature(Trim(text(block, "Feature")));
            const std::string offText(Trim(text(block, "Off")));
            const bool alwaysOn = Lower(std::string(Trim(text(block, "AlwaysOn")))) == "true" || Trim(text(block, "AlwaysOn")) == "1";

            if (type == "int")
            {
                const auto def_ = ini.Get<int>(block, "Default");
                const auto min = ini.Get<int>(block, "Min");
                const auto max = ini.Get<int>(block, "Max");
                if (!min || !max)
                {
                    mod->problems.push_back(where + ": an int needs Min and Max.");
                    continue;
                }
                if (*min > *max)
                {
                    mod->problems.push_back(where + ": Min is above Max.");
                    continue;
                }
                if (!def.empty() && !def_)
                {
                    mod->problems.push_back(where + ": Default is not a whole number.");
                    continue;
                }
                const int d = def_ ? (std::min)((std::max)(*def_, *min), *max) : *min;
                row.field = Field::Int(sectionText, keyText, helpText, d, *min, *max);
            }
            else if (type == "bool")
            {
                const std::string lower = Lower(def);
                const bool d = lower == "1" || lower == "true" || lower == "yes" || lower == "on";
                if (!def.empty() && !d && !(lower == "0" || lower == "false" || lower == "no" || lower == "off"))
                {
                    mod->problems.push_back(where + ": Default is not true/false (or 1/0).");
                    continue;
                }
                row.field = Field::Bool(sectionText, keyText, helpText, d);
                const std::vector<std::string> pair = SplitBar(text(block, "BoolText"));
                if (!pair.empty())
                {
                    if (pair.size() != 2)
                    {
                        mod->problems.push_back(where + ": BoolText needs two values, true then false, with | between.");
                        continue;
                    }
                    row.trueText = Keep(*mod, pair[0]);
                    row.falseText = Keep(*mod, pair[1]);
                }
            }
            else if (type == "choice")
            {
                const std::vector<std::string> choices = SplitBar(text(block, "Choices"));
                if (choices.size() < 2)
                {
                    mod->problems.push_back(where + ": a choice needs at least two Choices, with | between.");
                    continue;
                }
                std::vector<const char*> kept;
                const char* defText = nullptr;
                for (const std::string& c : choices)
                {
                    kept.push_back(Keep(*mod, c));
                    if (c == def)
                    {
                        defText = kept.back();
                    }
                }
                if (!def.empty() && !defText)
                {
                    mod->problems.push_back(where + ": Default is not one of the Choices.");
                    continue;
                }
                row.field = Field::Choice(sectionText, keyText, helpText, defText ? defText : kept.front(), std::move(kept));
            }
            else if (type.empty())
            {
                mod->problems.push_back(where + ": no Type (int, bool or choice).");
                continue;
            }
            else
            {
                mod->problems.push_back(where + ": Type \"" + type + "\" is not int, bool or choice.");
                continue;
            }
            if (!feature.empty())
            {
                row.feature = Keep(*mod, feature);
                row.alwaysOn = alwaysOn;
                if (!offText.empty())
                {
                    row.offText = Keep(*mod, offText);
                }
                else if (type == "bool")
                {
                    row.offText = row.falseText;
                }
                else if (!alwaysOn)
                {
                    mod->problems.push_back(where + ": Feature needs Off (the value at which the mod does nothing) or AlwaysOn = true.");
                    row.feature = "";
                }
            }
            mod->keys.push_back(std::move(row));
        }

        if (mod->keys.empty() && order.empty())
        {
            mod->problems.push_back("No settings: add a [Section/Key] block for each value the tab should edit.");
        }
        return mod;
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

        // Manifests first. One tab per ini name (case-insensitive): the first loadable copy
        // wins, a non-loadable one stands in until a loadable one turns up.
        std::map<std::string, size_t> byIni;   // lower-case ini name -> index into found
        for (const Place& place : places)
        {
            const std::filesystem::path dir = gameRoot / place.dir;
            if (!std::filesystem::is_directory(dir, ec))
            {
                continue;
            }
            std::vector<std::filesystem::path> manifests;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
            {
                if (entry.is_regular_file(ec) && EndsWithNoCase(entry.path().filename().string(), kManifestSuffix))
                {
                    manifests.push_back(entry.path());
                }
            }
            std::sort(manifests.begin(), manifests.end());
            for (const std::filesystem::path& manifest : manifests)
            {
                std::shared_ptr<Mod> mod = LoadManifest(manifest);
                if (!mod)
                {
                    continue;
                }
                Found f;
                f.mod = mod;
                bool iniPresent = false;
                if (*mod->fileName)
                {
                    // Beside the manifest, else in the game root; beside the manifest when
                    // neither exists yet (the tab creates it there on save).
                    f.file = dir / mod->fileName;
                    if (!std::filesystem::exists(f.file, ec) && std::filesystem::exists(gameRoot / mod->fileName, ec))
                    {
                        f.file = gameRoot / mod->fileName;
                    }
                    iniPresent = std::filesystem::exists(f.file, ec);
                }
                if (*mod->asiFile)
                {
                    // The .asi decides whether the mod runs: the first copy in a folder the
                    // game's loader scans, else wherever a copy sits (flagged), else nothing.
                    f.loadable = false;
                    for (const Place& where : places)
                    {
                        const std::filesystem::path asi = gameRoot / where.dir / mod->asiFile;
                        if (!std::filesystem::exists(asi, ec))
                        {
                            continue;
                        }
                        if (f.asi.empty() || (!f.loadable && where.loadable))
                        {
                            f.asi = asi;
                            f.loadable = where.loadable;
                        }
                        if (f.loadable)
                        {
                            break;
                        }
                    }
                    if (f.asi.empty())
                    {
                        f.loadable = place.loadable;   // no .asi anywhere: judge by the manifest's own folder
                    }
                }
                else
                {
                    f.loadable = true;
                }
                // A manifest describes a mod; with neither its ini nor its .asi anywhere, the
                // mod is not installed and there is nothing to edit - no tab. A manifest that
                // names no .asi stands on its ini alone. (Manifests shipped in the Enabler's
                // own zip rely on this.)
                if (!iniPresent && f.asi.empty())
                {
                    continue;
                }
                const std::string id = Lower(*mod->fileName ? mod->fileName : manifest.filename().string());
                const auto seen = byIni.find(id);
                if (seen == byIni.end())
                {
                    byIni[id] = found.size();
                    found.push_back(std::move(f));
                }
                else if (!found[seen->second].loadable && f.loadable)
                {
                    found[seen->second] = std::move(f);
                }
            }
        }

        // Then the built-in table, for inis no manifest claimed.
        for (const Mod& mod : Known())
        {
            if (byIni.count(Lower(mod.fileName)))
            {
                continue;
            }
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
                f.mod = std::shared_ptr<const Mod>(std::shared_ptr<const Mod>(), &mod);   // the table outlives everything
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
        m_Lines = SplitLines(content);

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

    std::optional<std::string> File::Save()
    {
        if (m_Path.empty())
        {
            return "This tab has no file to save to: its manifest names no Ini.";
        }

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
                    // A value the file had in quotes stays in quotes: some mods' readers want
                    // them (and a value with spaces needs them).
                    const std::string_view rest = Trim(std::string_view(line).substr(cut));
                    const bool quoted = rest.size() >= 2 && rest.front() == '"' && rest.back() == '"';
                    m_Lines[i] = line.substr(0, cut) + (quoted ? "\"" + value + "\"" : value);
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
