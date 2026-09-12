#include "pch.hpp"

#include "games.hpp"
#include "paths.hpp"
#include "settings_io.hpp"
#include "ui.hpp"
#include "compat.hpp"
#include "version.hpp"

namespace mgs4e::tool
{
    class App : public wxApp
    {
    public:
        bool OnInit() override
        {
            SetVendorName(MGS4E_COMPANY_NAME);
            wxImage::AddHandler(new wxPNGHandler);
            MSWEnableDarkMode();

            // "--game MGS4|MGSPW" picks the game when the tool sits where both could be found;
            // "--tab <title>" opens on that tab, for screenshots and support.
            std::string preferred;
            std::string openTab;
            for (int i = 1; i + 1 < argc; ++i)
            {
                if (wxString(argv[i]) == "--game")
                {
                    preferred = wxString(argv[i + 1]).ToStdString();
                }
                else if (wxString(argv[i]) == "--tab")
                {
                    openTab = wxString(argv[i + 1]).ToStdString();
                }
            }

            const auto install = FindInstalledGame(preferred);
            if (!install)
            {
                wxString hints;
                for (const Game* g : AllGames())
                {
                    hints += wxString::Format("\n  - %s: %s", g->gameTitle, g->notFoundHint);
                }
                wxMessageBox(
                    wxString::Format("Put \"%s.exe\" in the game's install folder and run it from there:%s\n\n"
                                     "Steam: right-click the game > Manage > Browse local files.", MGS4E_DISPLAY_NAME, hints),
                    wxString::Format("%s: game not found", MGS4E_DISPLAY_NAME), wxOK | wxICON_ERROR);
                return false;
            }
            const Game& game = *install->game;
            const std::filesystem::path& root = install->root;
            SetAppName(game.displayName);

            compat::Detect(game, root);

            Settings settings(game);
            std::string note;
            const std::filesystem::path file = Settings::FileFor(game, root);
            if (!settings.Load(file))
            {
                // First run. Earlier builds of these fixes shipped inside another mod and kept
                // their settings in its file; carry those across so nothing is lost.
                int imported = 0;
                std::string from;
                for (const compat::Status& other : compat::All())
                {
                    if (!other.settingsFound)
                    {
                        continue;
                    }
                    const int n = settings.ImportFrom(other.settingsPath);
                    if (n > 0)
                    {
                        imported += n;
                        from = other.settingsPath.filename().string();
                    }
                }
                note = imported > 0
                    ? "Imported " + std::to_string(imported) + " settings from " + from + ". Save to keep them."
                    : "First run: showing the defaults. Save to create " + std::string(game.name) + ".settings.";
            }

            MainFrame* frame = new MainFrame(game, root, std::move(settings), std::move(note), std::move(openTab));
            frame->Show();
            return true;
        }
    };
}

wxIMPLEMENT_APP(mgs4e::tool::App);
