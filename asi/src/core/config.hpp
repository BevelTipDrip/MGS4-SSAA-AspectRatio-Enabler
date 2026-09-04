#pragma once

#include <filesystem>

// PFCompanion.settings, and the lab file that replaces it for research boots.
//
// Load() reads the file once and writes every value straight into the feature namespaces
// (RenderPipeline, GraphicsSettings via RenderPipeline, AspectRatio, StageAutomation). A
// missing file is not an error: every setting has a default that leaves the game alone.
namespace pfc::config
{
    void Load();

    // Whether this boot is a lab run: the harness dropped a fresh PFCompanion.lab marker next
    // to a PFCompanion.lab.settings, so that file was read instead of the user's and the
    // research keys were honoured. The marker is consumed as it is read.
    bool LabMode();

    // The settings file that was (or would have been) read.
    const std::filesystem::path& File();
}
