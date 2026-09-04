#include "pch.hpp"
#include "ui.hpp"

#include "upstream.hpp"
#include "ini.hpp"
#include "settings_keys.hpp"
#include "version.hpp"

namespace pfc::tool
{
    namespace
    {
        // The banner's right-hand colour; the strip behind it is painted the same so the
        // image can sit left-aligned in a window of any width without a visible seam.
        const wxColour kBannerGround(26, 30, 36);
        // Brass rule under the banner (the art script's "brassLo").
        const wxColour kBannerRule(150, 108, 36);

        enum
        {
            ID_ResetToDefaults = wxID_HIGHEST + 1,
            ID_Save,
            ID_SaveAndExit,
            ID_OpenLogs,
        };

        // "Window Resolution (16:9)" -> "Window Resolution", for a switched row's shared label.
        wxString CommonLabel(const std::vector<const Field*>& fields)
        {
            wxString first = fields.front()->key;
            const size_t paren = first.find(" (");
            return paren == wxString::npos ? first : first.substr(0, paren);
        }

        bool IsTrue(const std::string& text)
        {
            const auto b = pfc::Ini::Convert<bool>(text);
            return b && *b;
        }

        // A group's fields as rows: consecutive fields shown by the same parent collapse into
        // one switched row (the per-shape window resolutions).
        std::vector<std::vector<const Field*>> RowsOf(const Group& group)
        {
            std::vector<std::vector<const Field*>> rows;
            for (size_t i = 0; i < group.fields.size();)
            {
                const Field& f = group.fields[i];
                std::vector<const Field*> row { &f };
                size_t j = i + 1;
                if (f.parentShows)
                {
                    while (j < group.fields.size() && group.fields[j].parentShows
                           && std::string_view(group.fields[j].parentKey) == f.parentKey)
                    {
                        row.push_back(&group.fields[j]);
                        ++j;
                    }
                }
                rows.push_back(std::move(row));
                i = j;
            }
            return rows;
        }

        int RowCount(const Group& group)
        {
            return static_cast<int>(RowsOf(group).size());
        }
    }

    MainFrame::MainFrame(const std::filesystem::path& gameRoot, Settings settings, std::string openingNote)
        : wxFrame(nullptr, wxID_ANY, wxString::Format("%s %s", PFC_DISPLAY_NAME, PFC_VERSION_STRING),
                  wxDefaultPosition, wxDefaultSize,
                  wxDEFAULT_FRAME_STYLE & ~(wxRESIZE_BORDER | wxMAXIMIZE_BOX))
        , m_GameRoot(gameRoot)
        , m_File(Settings::FileFor(gameRoot))
        , m_Settings(std::move(settings))
    {
        SetIcons(wxIconBundle("IDI_ICON1", wxGetInstance()));

        wxToolTip::Enable(true);
        wxToolTip::SetAutoPop(60000);
        wxToolTip::SetMaxWidth(FromDIP(560));

        wxPanel* root = new wxPanel(this);
        wxBoxSizer* rootSizer = new wxBoxSizer(wxVERTICAL);

        // Banner strip: the PNG sits at the left; the strip's own ground and a brass rule
        // continue it across whatever width the window ends up.
        wxPanel* strip = new wxPanel(root);
        strip->SetBackgroundColour(kBannerGround);
        wxBoxSizer* stripSizer = new wxBoxSizer(wxVERTICAL);
        wxBitmap banner = wxBITMAP_PNG(banner);
        if (banner.IsOk())
        {
            stripSizer->Add(new wxStaticBitmap(strip, wxID_ANY, banner), 0);
        }
        wxPanel* rule = new wxPanel(strip, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(3)));
        rule->SetBackgroundColour(kBannerRule);
        stripSizer->Add(rule, 0, wxEXPAND);
        strip->SetSizer(stripSizer);
        rootSizer->Add(strip, 0, wxEXPAND);

        // Pages.
        wxNotebook* book = new wxNotebook(root, wxID_ANY);
        for (const Page& page : Pages())
        {
            if (std::string_view(page.title) == "Troubleshooting")
            {
                book->AddPage(BuildTroubleshootingPage(book, page), page.title);
            }
            else
            {
                book->AddPage(BuildGraphicsPage(book, page), page.title);
            }
        }
        book->AddPage(BuildAboutPage(book), "About");
        rootSizer->Add(book, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        // Footer: status on the left, buttons on the right.
        wxBoxSizer* footer = new wxBoxSizer(wxHORIZONTAL);
        m_Status = new wxStaticText(root, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
        footer->Add(m_Status, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        footer->Add(new wxButton(root, ID_ResetToDefaults, "Reset to Defaults"), 0, wxRIGHT, FromDIP(12));
        footer->Add(new wxButton(root, ID_Save, "Save"), 0, wxRIGHT, FromDIP(6));
        footer->Add(new wxButton(root, ID_SaveAndExit, "Save and Exit"), 0, wxRIGHT, FromDIP(6));
        footer->Add(new wxButton(root, wxID_EXIT, "Exit"), 0);
        rootSizer->Add(footer, 0, wxEXPAND | wxALL, FromDIP(10));

        root->SetSizer(rootSizer);

        Bind(wxEVT_BUTTON, &MainFrame::OnResetToDefaults, this, ID_ResetToDefaults);
        Bind(wxEVT_BUTTON, &MainFrame::OnSave, this, ID_Save);
        Bind(wxEVT_BUTTON, &MainFrame::OnSaveAndExit, this, ID_SaveAndExit);
        Bind(wxEVT_BUTTON, &MainFrame::OnExit, this, wxID_EXIT);
        Bind(wxEVT_BUTTON, &MainFrame::OnOpenLogs, this, ID_OpenLogs);
        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

        WriteControls();
        UpdateDependencies();

        wxBoxSizer* frameSizer = new wxBoxSizer(wxVERTICAL);
        frameSizer->Add(root, 1, wxEXPAND);
        SetSizerAndFit(frameSizer);
        Centre();

        SetStatus(openingNote.empty() ? wxString::Format("Settings file: %s", m_File.wstring()) : wxString(openingNote));
    }

    // --- Building ------------------------------------------------------------------------

    wxWindow* MainFrame::BuildGraphicsPage(wxWindow* parent, const Page& page)
    {
        wxPanel* panel = new wxPanel(parent);
        wxBoxSizer* outer = new wxBoxSizer(wxVERTICAL);

        if (upstream::Current().asiInstalled)
        {
            wxStaticText* note = new wxStaticText(panel, wxID_ANY,
                wxString::Format("%s is installed. Where both mods change the same thing, its setting is used: "
                                 "those fields are greyed out below and show the value it has.", PFC_NEIGHBOUR_NAME));
            note->Wrap(FromDIP(900));
            outer->Add(note, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        }

        // Groups flow into two columns in table order: the left takes groups until it holds
        // half the rows, the right takes the rest. A group's title counts as two rows.
        int totalRows = 0;
        for (const Group& g : page.groups)
        {
            totalRows += RowCount(g) + 2;
        }
        wxBoxSizer* columns = new wxBoxSizer(wxHORIZONTAL);
        wxBoxSizer* left = new wxBoxSizer(wxVERTICAL);
        wxBoxSizer* right = new wxBoxSizer(wxVERTICAL);
        int leftRows = 0;
        for (const Group& g : page.groups)
        {
            wxBoxSizer* column = (leftRows < totalRows / 2) ? left : right;
            if (column == left)
            {
                leftRows += RowCount(g) + 2;
            }
            column->Add(BuildGroup(panel, g), 0, wxEXPAND | wxBOTTOM, FromDIP(10));
        }
        columns->Add(left, 1, wxEXPAND | wxRIGHT, FromDIP(10));
        columns->Add(right, 1, wxEXPAND);
        outer->Add(columns, 1, wxEXPAND | wxALL, FromDIP(12));

        panel->SetSizer(outer);
        return panel;
    }

    wxWindow* MainFrame::BuildTroubleshootingPage(wxWindow* parent, const Page& page)
    {
        wxPanel* panel = new wxPanel(parent);
        wxBoxSizer* outer = new wxBoxSizer(wxVERTICAL);

        for (const Group& g : page.groups)
        {
            outer->Add(BuildGroup(panel, g), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        }

        wxStaticText* steps = new wxStaticText(panel, wxID_ANY,
            wxString::Format(
                "To capture a log for a bug report: tick Debug Logging, save, launch the game from Steam and "
                "reproduce the problem, then quit and attach logs\\%s_Game.log from the game's install folder. "
                "The log is rewritten on every launch, so copy it before starting the game again.\n"
                "\n"
                "For a HUD element in the wrong place on an ultrawide or 4:3 screen, press F11 in-game while it is on "
                "screen; a record of every HUD element being drawn at that moment goes into the log. Take a screenshot "
                "at the same time (Steam's F12) and attach both.\n"
                "\n"
                "Report problems at %s.", PFC_NAME, PFC_REPO_URL));
        steps->Wrap(FromDIP(620));
        outer->Add(steps, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        outer->Add(new wxButton(panel, ID_OpenLogs, "Open the logs folder"), 0, wxALL, FromDIP(12));

        panel->SetSizer(outer);
        return panel;
    }

    wxWindow* MainFrame::BuildAboutPage(wxWindow* parent)
    {
        wxPanel* panel = new wxPanel(parent);
        wxBoxSizer* outer = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer* head = new wxBoxSizer(wxHORIZONTAL);
        wxIcon mark("IDI_ICON1", wxBITMAP_TYPE_ICO_RESOURCE, FromDIP(48), FromDIP(48));
        if (mark.IsOk())
        {
            head->Add(new wxStaticBitmap(panel, wxID_ANY, wxBitmap(mark)), 0, wxRIGHT, FromDIP(12));
        }
        wxBoxSizer* titles = new wxBoxSizer(wxVERTICAL);
        wxStaticText* title = new wxStaticText(panel, wxID_ANY, wxString::Format("%s %s", PFC_DISPLAY_NAME, PFC_VERSION_STRING));
        title->SetFont(title->GetFont().Scaled(1.5f).Bold());
        titles->Add(title, 0, wxBOTTOM, FromDIP(2));
        titles->Add(new wxStaticText(panel, wxID_ANY, "Graphics settings for METAL GEAR SOLID 4 (Master Collection)"), 0);
        head->Add(titles, 0, wxALIGN_CENTER_VERTICAL);
        outer->Add(head, 0, wxALL, FromDIP(12));

        wxStaticText* body = new wxStaticText(panel, wxID_ANY,
            wxString::Format(
                "%s.asi patches the game as it starts: internal resolution scaling, sharper shadows, anisotropic "
                "filtering, FXAA control, and an undistorted HUD on ultrawide and 4:3 displays. This tool writes the "
                "settings it reads.\n"
                "\n"
                "It is built to run alongside %s, which handles the launcher, controller and mouse fixes. Install both; "
                "where the two change the same thing, %s takes precedence and this tool says so.\n"
                "\n"
                "Nothing here phones home. There is no updater and no telemetry; the only files touched are "
                "%s.settings and the log.",
                PFC_NAME, PFC_NEIGHBOUR_NAME, PFC_NEIGHBOUR_NAME, PFC_NAME));
        body->Wrap(FromDIP(620));
        outer->Add(body, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        outer->Add(new wxHyperlinkCtrl(panel, wxID_ANY, PFC_REPO_URL, PFC_REPO_URL), 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        wxStaticText* credits = new wxStaticText(panel, wxID_ANY,
            "Built with safetyhook, Zydis, spdlog and wxWidgets. ASI loading by ThirteenAG's Ultimate ASI Loader.\n"
            PFC_COPYRIGHT);
        credits->Wrap(FromDIP(620));
        outer->Add(credits, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        panel->SetSizer(outer);
        return panel;
    }

    wxSizer* MainFrame::BuildGroup(wxWindow* parent, const Group& group)
    {
        wxStaticBoxSizer* box = new wxStaticBoxSizer(wxVERTICAL, parent, group.title);
        wxWindow* boxParent = box->GetStaticBox();
        wxFlexGridSizer* grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
        grid->AddGrowableCol(0, 1);

        for (std::vector<const Field*>& rowFields : RowsOf(group))
        {
            AddRow(boxParent, grid, std::move(rowFields));
        }

        box->Add(grid, 1, wxEXPAND | wxALL, FromDIP(8));
        return box;
    }

    void MainFrame::AddRow(wxWindow* parent, wxFlexGridSizer* grid, std::vector<const Field*> fields)
    {
        Row row;
        row.fields = std::move(fields);
        const Field& first = *row.fields.front();
        row.override = upstream::Overrides(first.section, first.key);

        row.label = new wxStaticText(parent, wxID_ANY, wxEmptyString);

        switch (first.type)
        {
        case Field::Type::Bool:
            row.control = new wxCheckBox(parent, wxID_ANY, wxEmptyString);
            row.control->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { ReadControls(); });
            break;
        case Field::Type::Int:
        {
            wxSpinCtrl* spin = new wxSpinCtrl(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1),
                                              wxSP_ARROW_KEYS, first.min, first.max, first.defaultInt);
            spin->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { ReadControls(); });
            spin->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { ReadControls(); });
            row.control = spin;
            break;
        }
        case Field::Type::Choice:
        {
            wxChoice* choice = new wxChoice(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(190), -1));
            choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { ReadControls(); });
            row.control = choice;
            break;
        }
        }

        grid->Add(row.label, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(row.control, 0, wxALIGN_CENTER_VERTICAL);
        m_Rows.push_back(row);
    }

    // --- Dependencies --------------------------------------------------------------------

    std::string MainFrame::ParentValue(const Field& field) const
    {
        return m_Settings.Get(field.section, field.parentKey);
    }

    bool MainFrame::ParentAllows(const Field& field) const
    {
        if (!field.parentKey)
        {
            return true;
        }
        const std::string value = ParentValue(field);
        const Field* parent = FindField(field.section, field.parentKey);
        const std::string normalised = (parent && parent->type == Field::Type::Bool) ? (IsTrue(value) ? "true" : "false") : value;
        return std::find_if(field.parentValues.begin(), field.parentValues.end(),
                            [&](const char* v) { return normalised == v; }) != field.parentValues.end();
    }

    const Field* MainFrame::ActiveField(const Row& row) const
    {
        if (row.fields.size() == 1)
        {
            return row.fields.front();
        }
        for (const Field* f : row.fields)
        {
            if (ParentAllows(*f))
            {
                return f;
            }
        }
        return nullptr;
    }

    wxString MainFrame::LabelFor(const Row& row) const
    {
        wxString label = row.fields.size() == 1 ? wxString(row.fields.front()->key) : CommonLabel(row.fields);
        if (row.override)
        {
            const std::string& text = *row.override;
            const size_t eq = text.rfind(" = ");
            const std::string value = eq == std::string::npos ? text : text.substr(eq + 3);
            label += wxString::Format("  (%s: %s)", PFC_NEIGHBOUR_NAME, value);
        }
        return label;
    }

    wxString MainFrame::TooltipFor(const Row& row) const
    {
        const Field* active = ActiveField(row);
        const Field* shown = active ? active : row.fields.front();
        wxString tip = shown->help;
        if (row.override)
        {
            tip = wxString::Format("%s is installed and sets this itself (%s). %s ignores the value here while that is so; "
                                   "change it in %s's own settings.\n\n",
                                   PFC_NEIGHBOUR_NAME, *row.override, PFC_DISPLAY_NAME, PFC_NEIGHBOUR_NAME) + tip;
        }
        return tip;
    }

    void MainFrame::UpdateDependencies()
    {
        for (Row& row : m_Rows)
        {
            const Field* active = ActiveField(row);
            bool enabled = !row.override && active != nullptr && ParentAllows(*active);

            if (row.fields.size() > 1)
            {
                // Switched row: repopulate the list for whichever field the parent selects.
                wxChoice* choice = static_cast<wxChoice*>(row.control);
                m_Syncing = true;
                choice->Clear();
                if (active)
                {
                    for (const char* c : active->choices)
                    {
                        choice->Append(c);
                    }
                    choice->SetStringSelection(m_Settings.Get(active->section, active->key));
                }
                else
                {
                    choice->Append("Game setting");
                    choice->SetSelection(0);
                }
                m_Syncing = false;
            }

            row.label->SetLabel(LabelFor(row));
            row.label->Enable(enabled);
            row.control->Enable(enabled);
            const wxString tip = TooltipFor(row);
            row.label->SetToolTip(tip);
            row.control->SetToolTip(tip);
        }
        Layout();
    }

    // --- Sync ----------------------------------------------------------------------------

    void MainFrame::ReadControls()
    {
        if (m_Syncing)
        {
            return;
        }
        for (const Row& row : m_Rows)
        {
            const Field* f = ActiveField(row);
            if (!f)
            {
                continue;
            }
            std::string value;
            switch (f->type)
            {
            case Field::Type::Bool:
                value = static_cast<wxCheckBox*>(row.control)->GetValue() ? "true" : "false";
                break;
            case Field::Type::Int:
                value = std::to_string(static_cast<wxSpinCtrl*>(row.control)->GetValue());
                break;
            case Field::Type::Choice:
            {
                const wxString sel = static_cast<wxChoice*>(row.control)->GetStringSelection();
                if (sel.empty() || !f->Accepts(sel.ToStdString()))
                {
                    continue;
                }
                value = sel.ToStdString();
                break;
            }
            }
            m_Settings.Set(f->section, f->key, value);
        }
        UpdateDependencies();
    }

    void MainFrame::WriteControls()
    {
        m_Syncing = true;
        for (const Row& row : m_Rows)
        {
            const Field* f = ActiveField(row);
            if (!f)
            {
                continue;
            }
            const std::string value = m_Settings.Get(f->section, f->key);
            switch (f->type)
            {
            case Field::Type::Bool:
                static_cast<wxCheckBox*>(row.control)->SetValue(IsTrue(value));
                break;
            case Field::Type::Int:
                static_cast<wxSpinCtrl*>(row.control)->SetValue(f->Accepts(value) ? std::stoi(value) : f->defaultInt);
                break;
            case Field::Type::Choice:
            {
                wxChoice* choice = static_cast<wxChoice*>(row.control);
                if (row.fields.size() == 1 && choice->GetCount() == 0)
                {
                    for (const char* c : f->choices)
                    {
                        choice->Append(c);
                    }
                }
                if (row.fields.size() == 1 && !choice->SetStringSelection(value))
                {
                    choice->SetStringSelection(f->defaultChoice);
                }
                break;
            }
            }
        }
        m_Syncing = false;
    }

    // --- Actions -------------------------------------------------------------------------

    bool MainFrame::Save()
    {
        ReadControls();
        if (const auto error = m_Settings.Save(m_File))
        {
            wxMessageBox(*error, "Could not save", wxOK | wxICON_ERROR, this);
            return false;
        }
        // Reload so Dirty() compares against what is now on disk.
        m_Settings.Load(m_File);
        SetStatus(wxString::Format("Saved %s. Takes effect the next time the game starts.", m_File.filename().wstring()));
        return true;
    }

    void MainFrame::OnResetToDefaults(wxCommandEvent&)
    {
        m_Settings.ResetToDefaults();
        WriteControls();
        UpdateDependencies();
        SetStatus("Defaults restored. Save to keep them.");
    }

    void MainFrame::OnSave(wxCommandEvent&)
    {
        Save();
    }

    void MainFrame::OnSaveAndExit(wxCommandEvent&)
    {
        if (Save())
        {
            Destroy();
        }
    }

    void MainFrame::OnExit(wxCommandEvent&)
    {
        Close();
    }

    void MainFrame::OnClose(wxCloseEvent& event)
    {
        ReadControls();
        if (event.CanVeto() && m_Settings.Dirty())
        {
            const int answer = wxMessageBox("Save your changes before closing?", PFC_DISPLAY_NAME,
                                            wxYES_NO | wxCANCEL | wxICON_QUESTION, this);
            if (answer == wxCANCEL)
            {
                event.Veto();
                return;
            }
            if (answer == wxYES && !Save())
            {
                event.Veto();
                return;
            }
        }
        Destroy();
    }

    void MainFrame::OnOpenLogs(wxCommandEvent&)
    {
        const std::filesystem::path logs = m_GameRoot / "logs";
        std::error_code ec;
        std::filesystem::create_directories(logs, ec);
        wxLaunchDefaultApplication(logs.wstring());
    }

    void MainFrame::SetStatus(const wxString& text)
    {
        m_Status->SetLabel(text);
        m_Status->SetToolTip(text);
        Layout();
    }
}
