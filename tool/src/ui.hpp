#pragma once

#include "compat.hpp"
#include "fields.hpp"
#include "settings_io.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mgs4e::tool
{
    class MainFrame : public wxFrame
    {
    public:
        MainFrame(const std::filesystem::path& gameRoot, Settings settings, std::string openingNote);

    private:
        // One line of a group: a label and a control. A "switched" row carries several
        // fields that share a parent (the per-shape window resolutions) and shows whichever
        // one the parent selects, so the page keeps its shape as the choice changes.
        struct Row
        {
            std::vector<const Field*> fields;
            wxStaticText* label = nullptr;
            wxWindow* control = nullptr;
            std::optional<compat::Override> override;   // another mod controls this field; shown beside the label
        };

        wxWindow* BuildGraphicsPage(wxWindow* parent, const Page& page);
        wxWindow* BuildTroubleshootingPage(wxWindow* parent, const Page& page);
        wxWindow* BuildAboutPage(wxWindow* parent);
        wxSizer* BuildGroup(wxWindow* parent, const Group& group);
        void AddRow(wxWindow* parent, wxFlexGridSizer* grid, std::vector<const Field*> fields);

        const Field* ActiveField(const Row& row) const;
        std::string ParentValue(const Field& field) const;
        bool ParentAllows(const Field& field) const;
        wxString LabelFor(const Row& row) const;
        wxString TooltipFor(const Row& row) const;

        // Controls -> settings, then dependencies.
        void ReadControls();
        // Settings -> controls.
        void WriteControls();
        void UpdateDependencies();

        bool Save();
        void OnResetToDefaults(wxCommandEvent&);
        void OnSave(wxCommandEvent&);
        void OnSaveAndExit(wxCommandEvent&);
        void OnExit(wxCommandEvent&);
        void OnClose(wxCloseEvent&);
        void OnOpenLogs(wxCommandEvent&);

        void SetStatus(const wxString& text);

        std::filesystem::path m_GameRoot;
        std::filesystem::path m_File;
        Settings m_Settings;
        std::vector<Row> m_Rows;
        wxStaticText* m_Status = nullptr;
        bool m_Syncing = false;
    };
}
