#include "pch.hpp"
#include "games.hpp"

#include "pw/settings_keys.hpp"
#include "pw/identity.hpp"

// The Peace Walker settings the tool edits. Nothing patches the game yet; the tab set is the
// logging switch until the engine research (docs/pw) promotes settings.
namespace mgs4e::tool::pw
{
    namespace
    {
        namespace K = mgspwe::keys;
        using F = Field;

        constexpr const char* kHelp_DebugLogging =
            "Writes more detail to logs\\" MGSPWE_NAME "_Game.log: which hooks were installed and what each setting resolved to.\n"
            "\n"
            "Turn it on before reproducing a problem, then attach the log to your report.";

        std::vector<Page> BuildPages()
        {
            const char* D = K::Debugging;
            return {
                { "Troubleshooting", {
                    { "Logging", {
                        F::Bool(D, K::DebugLogging, kHelp_DebugLogging, false),
                    }},
                }},
            };
        }
    }

    const std::vector<Page>& Pages()
    {
        static const std::vector<Page> pages = BuildPages();
        return pages;
    }

    const std::vector<const Field*>& AllFields()
    {
        static const std::vector<const Field*> all = []
        {
            std::vector<const Field*> v;
            for (const Page& p : Pages())
            {
                for (const Group& g : p.groups)
                {
                    for (const Field& f : g.fields)
                    {
                        v.push_back(&f);
                    }
                }
            }
            return v;
        }();
        return all;
    }

    const Field* FindField(const std::string& section, const std::string& key)
    {
        for (const Field* f : AllFields())
        {
            if (section == f->section && key == f->key)
            {
                return f;
            }
        }
        return nullptr;
    }
}
