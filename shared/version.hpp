#pragma once

// PF Companion version and naming. Shared by the ASI and the settings tool.
//
// PFC_NAME drives file names: PFCompanion.asi, PFCompanion.settings, PFCompanion.lab and the
// log. PFC_DISPLAY_NAME is what people see in window titles and file properties.

#define PFC_NAME          "PFCompanion"
#define PFC_DISPLAY_NAME  "PF Companion"
#define PFC_REPO_URL      "https://github.com/BevelTipDrip/PFCompanion"

#define PFC_VERSION_MAJOR 0
#define PFC_VERSION_MINOR 0
#define PFC_VERSION_PATCH 1

#define PFC_STRINGIFY_(x) #x
#define PFC_STRINGIFY(x) PFC_STRINGIFY_(x)
#define PFC_VERSION_STRING PFC_STRINGIFY(PFC_VERSION_MAJOR) "." PFC_STRINGIFY(PFC_VERSION_MINOR) "." PFC_STRINGIFY(PFC_VERSION_PATCH)

// Resource metadata
#define PFC_COMPANY_NAME     "BevelTipDrip"
#define PFC_COPYRIGHT        "\xA9 2026 BevelTipDrip. MIT License."
#define PFC_FILE_VERSION_RC  PFC_VERSION_MAJOR,PFC_VERSION_MINOR,PFC_VERSION_PATCH,0

// The other mod PF Companion runs alongside. Its ASI loads before ours (alphabetical), and
// where both patch the same thing its setting wins - see overlap_table.hpp.
#define PFC_NEIGHBOUR_NAME          "MGSPatriotFix"
#define PFC_NEIGHBOUR_ASI           "MGSPatriotFix.asi"
#define PFC_NEIGHBOUR_SETTINGS_FILE "MGSPatriotFix.settings"
