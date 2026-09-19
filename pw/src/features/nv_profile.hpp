#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The NVIDIA driver's per-game profile: the store behind NVIDIA Control Panel > Manage 3D settings >
// Program Settings. Reached through nvapi64.dll, loaded at run time from System32 and only when a
// caller asks, so nothing here runs on an AMD or Intel system beyond one failed LoadLibrary.
//
// No NvAPI SDK: every entry point comes from nvapi_QueryInterface by its published id
// (NVIDIA/nvapi, nvapi_interface.h) and the structures mirror nvapi.h.
//
// This file and nv_profile.cpp use nothing of the plugin (no spdlog, no game headers), so the same
// source builds into a console test that runs against the real driver without starting the game.
namespace NvProfile
{
    // nvapi_QueryInterface(id) after NvAPI_Initialize; nullptr when there is no NVIDIA driver or the
    // driver does not have the entry point. Safe to call many times: the library is loaded once.
    void* Interface(uint32_t id);

    struct Outcome
    {
        bool nvidia = false;              // nvapi64.dll answered
        bool changed = false;             // the driver's store was written (never true on a dry run)
        std::vector<std::string> lines;   // what was found and done, for the log
    };

    // wanted = true: make Vertical sync = Fast in the profile that matches this exe (creating a
    // profile when the exe has none), and record what it replaced in stateFile.
    // wanted = false: if stateFile exists, put that back and delete the file. With no state file
    // this returns at once without loading anything: a Fast Sync value that did not come from here
    // is never touched.
    // dryRun: do everything inside the driver session but never save it, and never write or delete
    // stateFile. The session is discarded, so the store is left exactly as it was.
    Outcome FastSync(bool wanted, const std::filesystem::path& exe, const std::filesystem::path& stateFile, bool dryRun = false);
}
