#include "pch.hpp"

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
            SetAppName(MGS4E_DISPLAY_NAME);
            SetVendorName(MGS4E_COMPANY_NAME);
            wxImage::AddHandler(new wxPNGHandler);
            MSWEnableDarkMode();

            const auto root = FindGameRoot();
            if (!root)
            {
                wxMessageBox(
                    wxString::Format("Put \"%s.exe\" in the game's install folder - the one that contains the MGS4 folder "
                                     "with mgs4.exe in it - and run it from there.\n\n"
                                     "Steam: right-click the game > Manage > Browse local files.", MGS4E_DISPLAY_NAME),
                    wxString::Format("%s: game not found", MGS4E_DISPLAY_NAME), wxOK | wxICON_ERROR);
                return false;
            }

            compat::Detect(*root);

            Settings settings;
            std::string note;
            const std::filesystem::path file = Settings::FileFor(*root);
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
                    : "First run: showing the defaults. Save to create " MGS4E_NAME ".settings.";
            }

            // "--tab <title>" opens on that tab, for screenshots and support.
            std::string openTab;
            for (int i = 1; i + 1 < argc; ++i)
            {
                if (wxString(argv[i]) == "--tab")
                {
                    openTab = wxString(argv[i + 1]).ToStdString();
                }
            }

            MainFrame* frame = new MainFrame(*root, std::move(settings), std::move(note), std::move(openTab));
            frame->Show();
            return true;
        }
    };
}

wxIMPLEMENT_APP(mgs4e::tool::App);
