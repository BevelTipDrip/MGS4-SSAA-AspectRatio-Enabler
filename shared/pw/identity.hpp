#pragma once

// Identity of the Peace Walker build (MGSPWEnabler): the names the ASI, its settings file,
// its lab marker, its log and the tool use for it. The version is shared with the MGS4 build
// (shared/version.hpp: one version line, one changelog, one zip), so there are no version
// numbers here. The exe folder and name are what the tool looks for to know it sits in a
// Peace Walker install; the exe name has spaces and is compared case-insensitively.
#define MGSPWE_NAME          "MGSPWEnabler"
#define MGSPWE_DISPLAY_NAME  "MGS Peace Walker SSAA and Aspect Ratio Enabler"
#define MGSPWE_GAME_TITLE    "METAL GEAR SOLID: Peace Walker"
#define MGSPWE_REPO_URL      "https://github.com/BevelTipDrip/MGS4-SSAA-AspectRatio-Enabler"
#define MGSPWE_STEAM_APP_ID  "2492660"
#define MGSPWE_EXE_DIR       "mgspw"
#define MGSPWE_EXE_NAME      "METAL GEAR SOLID PEACE WALKER.exe"
#define MGSPWE_LICENSE_NOTE  "MIT. Peace Walker aspect fixes, when present, are a separately licensed module by BevelTipDrip (credit required)."
