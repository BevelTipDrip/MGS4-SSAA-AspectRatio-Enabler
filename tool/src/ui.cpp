#include "pch.hpp"
#include "ui.hpp"

#include "compat.hpp"
#include "features.hpp"
#include "games.hpp"
#include "ini.hpp"
#include "settings_keys.hpp"
#include "version.hpp"

#include <shellapi.h>

namespace mgs4e::tool
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
            ID_Launch,
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
            const auto b = mgs4e::Ini::Convert<bool>(text);
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

    MainFrame::MainFrame(const Game& game, const std::filesystem::path& gameRoot, Settings settings, std::string openingNote, std::string openTab)
        : wxFrame(nullptr, wxID_ANY, wxString::Format("%s %s", game.displayName, MGS4E_VERSION_STRING),
                  wxDefaultPosition, wxDefaultSize,
                  wxDEFAULT_FRAME_STYLE & ~(wxRESIZE_BORDER | wxMAXIMIZE_BOX))
        , m_Game(&game)
        , m_GameRoot(gameRoot)
        , m_File(Settings::FileFor(game, gameRoot))
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
        // The banner art is MGS4's; the other games get a text header on the same strip until
        // they have art of their own.
        wxBitmap banner = wxBITMAP_PNG(banner);
        if (std::string_view(game.id) == "MGS4" && banner.IsOk())
        {
            stripSizer->Add(new wxStaticBitmap(strip, wxID_ANY, banner), 0);
        }
        else
        {
            wxStaticText* head = new wxStaticText(strip, wxID_ANY, wxString(game.displayName).Upper());
            head->SetFont(head->GetFont().Scaled(1.8f).Bold());
            head->SetForegroundColour(wxColour(232, 226, 210));
            wxStaticText* sub = new wxStaticText(strip, wxID_ANY, game.aboutSubtitle);
            sub->SetForegroundColour(wxColour(170, 164, 150));
            stripSizer->Add(head, 0, wxLEFT | wxTOP, FromDIP(24));
            stripSizer->Add(sub, 0, wxLEFT | wxBOTTOM, FromDIP(24));
        }
        wxPanel* rule = new wxPanel(strip, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(3)));
        rule->SetBackgroundColour(kBannerRule);
        stripSizer->Add(rule, 0, wxEXPAND);
        strip->SetSizer(stripSizer);
        rootSizer->Add(strip, 0, wxEXPAND);

        // Pages.
        wxNotebook* book = new wxNotebook(root, wxID_ANY);
        for (const Page& page : m_Game->pages())
        {
            if (std::string_view(page.title) == "Troubleshooting")
            {
                // Other mods' config files get a tab each, ahead of Troubleshooting. Only the
                // files that exist: no mod, no tab.
                for (const modconfig::Found& found : modconfig::Detect(m_GameRoot, m_Game->manifestSuffix, m_Game->places))
                {
                    auto mod = std::make_unique<ModPage>();
                    mod->found = found;
                    mod->file.Load(found.file);
                    book->AddPage(BuildModPage(book, *mod), found.mod->name);
                    m_ModPages.push_back(std::move(mod));
                }
                book->AddPage(BuildTroubleshootingPage(book, page), page.title);
            }
            else
            {
                book->AddPage(BuildGraphicsPage(book, page), page.title);
            }
        }
        book->AddPage(BuildAboutPage(book), "About");
        rootSizer->Add(book, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        if (!openTab.empty())
        {
            // Once the frame is up: a selection made before the first show does not stick.
            CallAfter([book, openTab]
            {
                for (size_t i = 0; i < book->GetPageCount(); ++i)
                {
                    if (book->GetPageText(i) == openTab)
                    {
                        book->SetSelection(i);
                        break;
                    }
                }
            });
        }

        // Footer: status on the left, buttons on the right.
        wxBoxSizer* footer = new wxBoxSizer(wxHORIZONTAL);
        m_Status = new wxStaticText(root, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
        footer->Add(m_Status, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        footer->Add(new wxButton(root, ID_ResetToDefaults, "Reset to Defaults"), 0, wxRIGHT, FromDIP(12));
        footer->Add(new wxButton(root, ID_Save, "Save"), 0, wxRIGHT, FromDIP(6));
        footer->Add(new wxButton(root, ID_SaveAndExit, "Save and Exit"), 0, wxRIGHT, FromDIP(6));
        footer->Add(new wxButton(root, ID_Launch, "Save and Launch Game"), 0, wxRIGHT, FromDIP(6));
        footer->Add(new wxButton(root, wxID_EXIT, "Exit"), 0);
        rootSizer->Add(footer, 0, wxEXPAND | wxALL, FromDIP(10));

        root->SetSizer(rootSizer);

        Bind(wxEVT_BUTTON, &MainFrame::OnResetToDefaults, this, ID_ResetToDefaults);
        Bind(wxEVT_BUTTON, &MainFrame::OnSave, this, ID_Save);
        Bind(wxEVT_BUTTON, &MainFrame::OnSaveAndExit, this, ID_SaveAndExit);
        Bind(wxEVT_BUTTON, &MainFrame::OnLaunch, this, ID_Launch);
        Bind(wxEVT_BUTTON, &MainFrame::OnExit, this, wxID_EXIT);
        Bind(wxEVT_BUTTON, &MainFrame::OnOpenLogs, this, ID_OpenLogs);
        Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

        WriteControls();
        UpdateDependencies();
        RefreshOverlapNotes();

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

        const auto installed = compat::Installed();
        if (!installed.empty())
        {
            wxString names;
            for (const compat::Status* s : installed)
            {
                names += (names.empty() ? "" : ", ") + wxString(s->name);
            }
            wxStaticText* note = new wxStaticText(panel, wxID_ANY,
                wxString::Format("Mod compatibility: %s is installed. Where both mods change the same thing, the other "
                                 "mod's setting is used: those fields are greyed out below and show the value it has.", names));
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
            wxString::Format(m_Game->troubleshootingText, m_Game->name, m_Game->repoUrl));
        steps->Wrap(FromDIP(620));
        outer->Add(steps, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

        outer->Add(new wxButton(panel, ID_OpenLogs, "Open the logs folder"), 0, wxALL, FromDIP(12));

        panel->SetSizer(outer);
        return panel;
    }

    wxWindow* MainFrame::BuildModPage(wxWindow* parent, ModPage& page)
    {
        const modconfig::Mod& mod = *page.found.mod;
        wxPanel* panel = new wxPanel(parent);
        wxBoxSizer* outer = new wxBoxSizer(wxVERTICAL);

        std::error_code ec;
        const auto relative = [&](const std::filesystem::path& path) -> std::wstring
        {
            const std::filesystem::path shown = std::filesystem::relative(path, m_GameRoot, ec);
            return (ec || shown.empty() ? path : shown).wstring();
        };
        wxString text = *mod.blurb ? wxString(mod.blurb) + "\n\n" : wxString();
        if (page.found.file.empty())
        {
            text += "File: none (the manifest names no Ini)";
        }
        else
        {
            text += wxString::Format("File: %s", relative(page.found.file));
            if (!std::filesystem::exists(page.found.file, ec))
            {
                text += " (not there yet; it is created on save)";
            }
        }
        if (!mod.manifest.empty())
        {
            text += wxString::Format("\nDescribed by: %s", relative(mod.manifest));
        }
        wxStaticText* blurb = new wxStaticText(panel, wxID_ANY, text);
        blurb->Wrap(FromDIP(620));
        outer->Add(blurb, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        if (*mod.url)
        {
            outer->Add(new wxHyperlinkCtrl(panel, wxID_ANY, mod.url, mod.url), 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        }

        if (!page.found.loadable)
        {
            const wxString files = *mod.asiFile ? wxString::Format("%s and %s", mod.asiFile, mod.fileName) : wxString(mod.fileName);
            wxStaticText* warn = new wxStaticText(panel, wxID_ANY, wxString::Format(m_Game->loaderNote, mod.name, files));
            warn->Wrap(FromDIP(620));
            warn->SetForegroundColour(wxColour(214, 128, 44));
            outer->Add(warn, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        }

        if (!mod.problems.empty())
        {
            // The manifest's author is the audience here: say what is wrong, in its terms.
            wxString list = wxString::Format("The manifest %s has problems; the rows concerned are left out:", relative(mod.manifest));
            for (const std::string& problem : mod.problems)
            {
                list += "\n  - " + wxString(problem);
            }
            wxStaticText* warn = new wxStaticText(panel, wxID_ANY, list);
            warn->Wrap(FromDIP(620));
            warn->SetForegroundColour(wxColour(214, 128, 44));
            outer->Add(warn, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        }

        wxStaticBoxSizer* box = new wxStaticBoxSizer(wxVERTICAL, panel, "Settings");
        wxWindow* boxParent = box->GetStaticBox();
        wxFlexGridSizer* grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
        grid->AddGrowableCol(0, 1);
        for (const modconfig::Key& key : mod.keys)
        {
            const Field& f = key.field;
            ModRow row;
            row.key = &key;
            wxStaticText* label = new wxStaticText(boxParent, wxID_ANY, key.label);
            row.label = label;
            switch (f.type)
            {
            case Field::Type::Bool:
                row.control = new wxCheckBox(boxParent, wxID_ANY, wxEmptyString);
                row.control->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { ReadControls(); });
                break;
            case Field::Type::Int:
            {
                wxSpinCtrl* spin = new wxSpinCtrl(boxParent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1),
                                                  wxSP_ARROW_KEYS, f.min, f.max, f.defaultInt);
                spin->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { ReadControls(); });
                spin->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { ReadControls(); });
                row.control = spin;
                break;
            }
            case Field::Type::Choice:
            {
                wxChoice* choice = new wxChoice(boxParent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(190), -1));
                for (const char* c : f.choices)
                {
                    choice->Append(c);
                }
                choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { ReadControls(); });
                row.control = choice;
                break;
            }
            }
            label->SetToolTip(f.help);
            row.control->SetToolTip(f.help);
            grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
            grid->Add(row.control, 0, wxALIGN_CENTER_VERTICAL);
            page.rows.push_back(row);
        }
        box->Add(grid, 1, wxEXPAND | wxALL, FromDIP(8));
        outer->Add(box, 0, wxEXPAND | wxALL, FromDIP(12));

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
        wxStaticText* title = new wxStaticText(panel, wxID_ANY, wxString::Format("%s %s", m_Game->displayName, MGS4E_VERSION_STRING));
        title->SetFont(title->GetFont().Scaled(1.5f).Bold());
        titles->Add(title, 0, wxBOTTOM, FromDIP(2));
        titles->Add(new wxStaticText(panel, wxID_ANY, m_Game->aboutSubtitle), 0);
        head->Add(titles, 0, wxALIGN_CENTER_VERTICAL);
        outer->Add(head, 0, wxALL, FromDIP(12));

        wxStaticText* body = new wxStaticText(panel, wxID_ANY, wxString::Format(m_Game->aboutBody, m_Game->name, m_Game->name));
        body->Wrap(FromDIP(620));
        outer->Add(body, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        outer->Add(new wxHyperlinkCtrl(panel, wxID_ANY, m_Game->repoUrl, m_Game->repoUrl), 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        wxStaticText* credits = new wxStaticText(panel, wxID_ANY,
            wxString("Built with safetyhook, Zydis, spdlog and wxWidgets. ASI loading by ThirteenAG's Ultimate ASI Loader.\n"
                     MGS4E_COPYRIGHT " ") + m_Game->licenseNote);
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
        row.override = compat::Overrides(first.section, first.key);

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
        const Field* parent = m_Game->findField(field.section, field.parentKey);
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
            label += wxString::Format("  (%s: %s)", row.override->modName, row.override->value);
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
            tip = wxString::Format("%s is installed and sets this itself (%s = %s). %s ignores the value here while that is so; "
                                   "change it in %s's own settings.\n\n",
                                   row.override->modName, row.override->theirKey, row.override->value, m_Game->displayName,
                                   row.override->modName) + tip;
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
        for (auto& mod : m_ModPages)
        {
            for (const ModRow& row : mod->rows)
            {
                const Field& f = row.key->field;
                std::string value;
                switch (f.type)
                {
                case Field::Type::Bool:
                    value = static_cast<wxCheckBox*>(row.control)->GetValue() ? row.key->trueText : row.key->falseText;
                    break;
                case Field::Type::Int:
                    value = std::to_string(static_cast<wxSpinCtrl*>(row.control)->GetValue());
                    break;
                case Field::Type::Choice:
                {
                    const wxString sel = static_cast<wxChoice*>(row.control)->GetStringSelection();
                    if (sel.empty())
                    {
                        continue;
                    }
                    value = sel.ToStdString();
                    break;
                }
                }
                mod->file.Set(f.section, f.key, value);
            }
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
        for (auto& mod : m_ModPages)
        {
            for (const ModRow& row : mod->rows)
            {
                const Field& f = row.key->field;
                const std::string value = mod->file.Get(f.section, f.key);
                switch (f.type)
                {
                case Field::Type::Bool:
                    static_cast<wxCheckBox*>(row.control)->SetValue(value.empty() ? f.defaultBool : IsTrue(value));
                    break;
                case Field::Type::Int:
                    static_cast<wxSpinCtrl*>(row.control)->SetValue(f.Accepts(value) ? std::stoi(value) : f.defaultInt);
                    break;
                case Field::Type::Choice:
                {
                    wxChoice* choice = static_cast<wxChoice*>(row.control);
                    if (!choice->SetStringSelection(value))
                    {
                        choice->SetStringSelection(f.defaultChoice);
                    }
                    break;
                }
                }
            }
        }
        m_Syncing = false;
    }

    // --- Actions -------------------------------------------------------------------------

    bool MainFrame::Save()
    {
        ReadControls();
        if (!ResolveOverlaps())
        {
            return false;
        }
        if (const auto error = m_Settings.Save(m_File))
        {
            wxMessageBox(*error, "Could not save", wxOK | wxICON_ERROR, this);
            return false;
        }
        // Reload so Dirty() compares against what is now on disk.
        m_Settings.Load(m_File);
        wxString saved = m_File.filename().wstring();
        for (auto& mod : m_ModPages)
        {
            if (!mod->file.Dirty())
            {
                continue;
            }
            if (const auto error = mod->file.Save())
            {
                wxMessageBox(*error, "Could not save", wxOK | wxICON_ERROR, this);
                return false;
            }
            saved += " and ";
            saved += mod->file.Path().filename().wstring();
        }
        SetStatus(wxString::Format("Saved %s. Takes effect the next time the game starts.", saved));
        RefreshOverlapNotes();
        return true;
    }

    // ---- feature ownership -------------------------------------------------------------------

    std::vector<MainFrame::Owner> MainFrame::ActiveOwners()
    {
        std::vector<Owner> owners;
        for (Row& row : m_Rows)
        {
            const Field* f = ActiveField(row);
            if (!f) { continue; }
            const mgs4e::features::Ours* ours = m_Game->OurClaim(f->section, f->key);
            if (!ours) { continue; }
            const std::string value = m_Settings.Get(f->section, f->key);
            if (mgs4e::features::IsOff(value, ours->off)) { continue; }
            owners.push_back(Owner{ ours->feature, m_Game->displayName, f->key, value, false, &row, nullptr, nullptr });
        }
        for (auto& page : m_ModPages)
        {
            for (ModRow& row : page->rows)
            {
                const modconfig::Key& key = *row.key;
                if (!*key.feature) { continue; }
                const std::string value = page->file.Get(key.field.section, key.field.key);
                const bool on = page->found.asi.empty() && !page->found.loadable ? false
                    : (key.alwaysOn || !mgs4e::features::IsOff(value, key.offText));
                if (!on) { continue; }
                owners.push_back(Owner{ key.feature, page->found.mod->name, key.label,
                    key.alwaysOn ? std::string("always on") : value, key.alwaysOn, nullptr, page.get(), &row });
            }
        }
        return owners;
    }

    void MainFrame::RefreshOverlapNotes()
    {
        const std::vector<Owner> owners = ActiveOwners();
        // Our rows: their labels are rebuilt from LabelFor plus the note.
        for (Row& row : m_Rows)
        {
            wxString note;
            const Field* f = ActiveField(row);
            const mgs4e::features::Ours* ours = f ? m_Game->OurClaim(f->section, f->key) : nullptr;
            if (ours)
            {
                for (const Owner& o : owners)
                {
                    if (o.row == &row || o.feature != ours->feature) { continue; }
                    // The greyed-out note already names the mod that owns this outright.
                    if (row.override && o.modName == row.override->modName) { continue; }
                    note += note.empty() ? "  - also on in " : ", ";
                    note += wxString::Format("%s (%s)", o.modName, o.value);
                }
            }
            row.label->SetLabel(LabelFor(row) + note);
        }
        for (auto& page : m_ModPages)
        {
            for (ModRow& row : page->rows)
            {
                wxString note;
                if (*row.key->feature)
                {
                    for (const Owner& o : owners)
                    {
                        if (o.modRow == &row || o.feature != row.key->feature) { continue; }
                        note += note.empty() ? "  - also on in " : ", ";
                        note += wxString::Format("%s (%s)", o.modName, o.value);
                    }
                }
                row.label->SetLabel(wxString(row.key->label) + note);
                if (!note.empty())
                {
                    row.label->SetForegroundColour(wxColour(214, 128, 44));
                }
                else
                {
                    row.label->SetForegroundColour(wxNullColour);
                }
            }
        }
        Layout();
    }

    void MainFrame::SetOwnerOff(const Owner& owner)
    {
        if (owner.row)
        {
            const Field* f = ActiveField(*owner.row);
            const mgs4e::features::Ours* ours = f ? m_Game->OurClaim(f->section, f->key) : nullptr;
            if (f && ours)
            {
                m_Settings.Set(f->section, f->key, ours->off);
            }
        }
        else if (owner.page && owner.modRow)
        {
            const modconfig::Key& key = *owner.modRow->key;
            owner.page->file.Set(key.field.section, key.field.key, key.offText);
        }
    }

    bool MainFrame::ResolveOverlaps()
    {
        // Feature -> owners that are on. Two or more means a choice to make.
        std::vector<Owner> owners = ActiveOwners();
        std::vector<std::string> features;
        for (const Owner& o : owners)
        {
            if (std::find(features.begin(), features.end(), o.feature) == features.end())
            {
                features.push_back(o.feature);
            }
        }
        bool changed = false;
        for (const std::string& feature : features)
        {
            std::vector<const Owner*> on;
            for (const Owner& o : owners)
            {
                if (o.feature == feature) { on.push_back(&o); }
            }
            if (on.size() < 2) { continue; }

            const wxString name(std::string(mgs4e::features::DisplayName(feature)));
            const Owner* forced = nullptr;
            for (const Owner* o : on)
            {
                if (o->alwaysOn) { forced = o; break; }
            }
            if (forced)
            {
                // One of them cannot be turned off from here: it wins, the rest go off.
                wxString others;
                for (const Owner* o : on)
                {
                    if (o == forced) { continue; }
                    others += wxString::Format("\n  - %s: %s = %s", o->modName, o->label, o->value);
                }
                const int answer = wxMessageBox(
                    wxString::Format("%s is set by %s whenever it is installed (%s), and also here:%s\n\nTwo mods patching the same thing stack or fight at start-up. Turn the others off and keep %s's?",
                                     name, forced->modName, forced->label, others, forced->modName),
                    "More than one mod owns " + name, wxYES_NO | wxICON_QUESTION, this);
                if (answer != wxYES) { return false; }
                for (const Owner* o : on)
                {
                    if (o != forced) { SetOwnerOff(*o); changed = true; }
                }
                continue;
            }

            wxArrayString choices;
            for (const Owner* o : on)
            {
                choices.Add(wxString::Format("%s: %s = %s", o->modName, o->label, o->value));
            }
            wxSingleChoiceDialog dialog(this,
                wxString::Format("%s is turned on in more than one mod. Two mods patching the same thing stack or fight at start-up, so only one should have it.\n\nWhich one keeps it? The others are set to their off values.", name),
                "More than one mod owns " + name, choices);
            dialog.SetSelection(0);
            if (dialog.ShowModal() != wxID_OK) { return false; }
            const int keep = dialog.GetSelection();
            for (int i = 0; i < static_cast<int>(on.size()); ++i)
            {
                if (i != keep) { SetOwnerOff(*on[static_cast<size_t>(i)]); changed = true; }
            }
        }
        if (changed)
        {
            WriteControls();
            UpdateDependencies();
        }
        return true;
    }

    bool MainFrame::Dirty()
    {
        if (m_Settings.Dirty())
        {
            return true;
        }
        for (const auto& mod : m_ModPages)
        {
            if (mod->file.Dirty())
            {
                return true;
            }
        }
        return false;
    }

    void MainFrame::OnResetToDefaults(wxCommandEvent&)
    {
        m_Settings.ResetToDefaults();
        WriteControls();
        UpdateDependencies();
        SetStatus(m_ModPages.empty() ? "Defaults restored. Save to keep them."
                                     : "Defaults restored (this mod's settings only; other mods' tabs are untouched). Save to keep them.");
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

    void MainFrame::OnLaunch(wxCommandEvent&)
    {
        // Save first, so what the game reads is what is on screen; then start it the way
        // Steam's Play button does (through its launcher, with the user's launch options),
        // and leave this window up - the game reads the file once at start, so nothing here
        // needs to stay closed.
        ReadControls();
        if (Dirty() && !Save())
        {
            return;
        }
        const std::wstring url = std::wstring(L"steam://rungameid/") + m_Game->steamAppId;
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32)
        {
            wxMessageBox(wxString::Format("Steam did not take the launch request (%s). Is Steam installed and running?", url),
                         "Could not launch", wxOK | wxICON_ERROR, this);
            return;
        }
        SetStatus(wxString::Format("Launching %s through Steam.", m_Game->gameTitle));
    }

    void MainFrame::OnExit(wxCommandEvent&)
    {
        Close();
    }

    void MainFrame::OnClose(wxCloseEvent& event)
    {
        ReadControls();
        if (event.CanVeto() && Dirty())
        {
            const int answer = wxMessageBox("Save your changes before closing?", m_Game->displayName,
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
