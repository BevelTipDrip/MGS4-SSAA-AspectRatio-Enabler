#pragma once

#include "compat.hpp"
#include "fields.hpp"
#include "games.hpp"
#include "mod_config.hpp"
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
        MainFrame(const Game& game, const std::filesystem::path& gameRoot, Settings settings, std::string openingNote, std::string openTab);

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

        // Another mod's config file on its own tab: the file, and a row per listed key.
        struct ModRow
        {
            const modconfig::Key* key = nullptr;
            wxStaticText* label = nullptr;
            wxWindow* control = nullptr;
        };
        struct ModPage
        {
            modconfig::Found found;
            modconfig::File file;
            std::vector<ModRow> rows;
        };

        wxWindow* BuildGraphicsPage(wxWindow* parent, const Page& page);
        wxWindow* BuildTroubleshootingPage(wxWindow* parent, const Page& page);
        wxWindow* BuildModPage(wxWindow* parent, ModPage& page);
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
        bool Dirty();

        // Feature ownership (shared/features.hpp): every setting, ours or a manifest mod's,
        // that is on for a feature. Overlaps are shown beside the rows concerned and, at
        // save time, resolved by asking which mod keeps each feature.
        struct Owner
        {
            std::string feature;
            std::string modName;    // the game's displayName for ours
            std::string label;      // the row's label
            std::string value;
            bool alwaysOn = false;
            Row* row = nullptr;         // ours, or
            ModPage* page = nullptr;    // theirs
            ModRow* modRow = nullptr;
        };
        std::vector<Owner> ActiveOwners();
        void RefreshOverlapNotes();
        bool ResolveOverlaps();
        void SetOwnerOff(const Owner& owner);
        void OnResetToDefaults(wxCommandEvent&);
        void OnSave(wxCommandEvent&);
        void OnSaveAndExit(wxCommandEvent&);
        void OnLaunch(wxCommandEvent&);
        void OnExit(wxCommandEvent&);
        void OnClose(wxCloseEvent&);
        void OnOpenLogs(wxCommandEvent&);

        void SetStatus(const wxString& text);

        const Game* m_Game = nullptr;
        std::filesystem::path m_GameRoot;
        std::filesystem::path m_File;
        Settings m_Settings;
        std::vector<Row> m_Rows;
        std::vector<std::unique_ptr<ModPage>> m_ModPages;   // stable addresses: rows bind to them
        wxStaticText* m_Status = nullptr;
        bool m_Syncing = false;
    };
}
