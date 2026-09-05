#pragma once

// MGS4 SSAA and Aspect Ratio Enabler: version and naming. Shared by the ASI and the settings
// tool.
//
// MGS4E_NAME drives file names: MGS4Enabler.asi, MGS4Enabler.settings, MGS4Enabler.lab, the
// log and the tool's exe. MGS4E_DISPLAY_NAME is what people see in window titles and file
// properties.

#define MGS4E_NAME          "MGS4Enabler"
#define MGS4E_DISPLAY_NAME  "MGS4 SSAA and Aspect Ratio Enabler"
#define MGS4E_REPO_URL      "https://github.com/BevelTipDrip/MGS4-SSAA-AspectRatio-Enabler"

#define MGS4E_VERSION_MAJOR 0
#define MGS4E_VERSION_MINOR 0
#define MGS4E_VERSION_PATCH 2

#define MGS4E_STRINGIFY_(x) #x
#define MGS4E_STRINGIFY(x) MGS4E_STRINGIFY_(x)
#define MGS4E_VERSION_STRING MGS4E_STRINGIFY(MGS4E_VERSION_MAJOR) "." MGS4E_STRINGIFY(MGS4E_VERSION_MINOR) "." MGS4E_STRINGIFY(MGS4E_VERSION_PATCH)

// Resource metadata
#define MGS4E_COMPANY_NAME     "BevelTipDrip"
#define MGS4E_COPYRIGHT        "\xA9 2026 BevelTipDrip."
#define MGS4E_LICENSE_NOTE     "Public source under the MIT License. The ultrawide and 4:3 fix is licensed separately: free to use, but it must be credited directly to BevelTipDrip."
#define MGS4E_FILE_VERSION_RC  MGS4E_VERSION_MAJOR,MGS4E_VERSION_MINOR,MGS4E_VERSION_PATCH,0

// Other mods this one knows how to share the game with are listed in compat_table.hpp.
