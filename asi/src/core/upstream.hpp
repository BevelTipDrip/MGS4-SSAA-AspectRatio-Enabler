#pragma once

#include <string>

// The neighbour: MGSPatriotFix, which PF Companion is designed to run alongside.
//
// Its ASI loads before ours (the loader goes alphabetically) and installs its hooks
// synchronously, so by the time we run its patches are in place. Where both mods patch the
// same thing, its setting wins and ours is skipped - see shared/overlap_table.hpp for the
// list and the reasoning. Its settings file is only ever read.
namespace pfc::upstream
{
    // Looks for the ASI in the process (or on disk, for the file-only case) and reads its
    // settings file if there is one. Called once after our own settings are loaded.
    void Detect();

    // Whether MGSPatriotFix.asi is loaded into this process.
    bool Loaded();

    // Whether our setting `key` in `section` should stand down because the neighbour is
    // patching the same thing. Logs the decision the first time it says yes for a key.
    bool Yields(const char* section, const char* key);

    // The raw value of one of the neighbour's settings, or empty when absent.
    std::string Value(const char* theirSection, const char* theirKey);
}
