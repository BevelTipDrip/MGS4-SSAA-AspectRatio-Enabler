#pragma once

// The Peace Walker build's view of "version.hpp".
//
// The shared core sources (asi/src/core/log.cpp, game.cpp, mem.cpp) are written against the
// MGS4E_* macros and compiled into MGSPWEnabler unchanged; this header is what they see,
// because $(ProjectDir)src comes before shared\ on this project's include path (see
// MGSPWEnabler.vcxproj). It takes the real shared/version.hpp first - the version numbers
// and the stringify machinery are shared, one version line for both games - and re-points
// only the identity names at the Peace Walker values from shared/pw/identity.hpp. Peace
// Walker code of its own uses the MGSPWE_* names directly.
#include "../../shared/version.hpp"
#include "pw/identity.hpp"

#undef MGS4E_NAME
#undef MGS4E_DISPLAY_NAME
#undef MGS4E_REPO_URL
#undef MGS4E_STEAM_APP_ID
#undef MGS4E_LICENSE_NOTE

#define MGS4E_NAME          MGSPWE_NAME
#define MGS4E_DISPLAY_NAME  MGSPWE_DISPLAY_NAME
#define MGS4E_REPO_URL      MGSPWE_REPO_URL
#define MGS4E_STEAM_APP_ID  MGSPWE_STEAM_APP_ID
#define MGS4E_LICENSE_NOTE  MGSPWE_LICENSE_NOTE
