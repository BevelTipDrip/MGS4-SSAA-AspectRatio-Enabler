#include "pch.hpp"

#include "paths.hpp"
#include "settings_io.hpp"
#include "ui.hpp"
#include "upstream.hpp"
#include "version.hpp"

namespace pfc::tool
{
    class App : public wxApp
    {
    public:
        bool OnInit() override
        {
            SetAppName(PFC_DISPLAY_NAME);
            SetVendorName(PFC_COMPANY_NAME);
            wxImage::AddHandler(new wxPNGHandler);
            MSWEnableDarkMode();

            const auto root = FindGameRoot();
            if (!root)
            {
                wxMessageBox(
                    wxString::Format("Put \"%s.exe\" in the game's install folder - the one that contains the MGS4 folder "
                                     "with mgs4.exe in it - and run it from there.\n\n"
                                     "Steam: right-click the game > Manage > Browse local files.", PFC_DISPLAY_NAME),
                    wxString::Format("%s: game not found", PFC_DISPLAY_NAME), wxOK | wxICON_ERROR);
                return false;
            }

            upstream::Detect(*root);

            Settings settings;
            std::string note;
            const std::filesystem::path file = Settings::FileFor(*root);
            if (!settings.Load(file))
            {
                // First run. Earlier builds of these fixes lived inside a fork of MGSPatriotFix
                // and kept their settings in its file; carry those across so nothing is lost.
                const int imported = settings.ImportFrom(upstream::Current().settingsPath);
                note = imported > 0
                    ? "Imported " + std::to_string(imported) + " settings from " PFC_NEIGHBOUR_SETTINGS_FILE
                      + ". Save to keep them."
                    : "First run: showing the defaults. Save to create " PFC_NAME ".settings.";
            }

            MainFrame* frame = new MainFrame(*root, std::move(settings), std::move(note));
            frame->Show();
            return true;
        }
    };
}

wxIMPLEMENT_APP(pfc::tool::App);
