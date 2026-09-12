#pragma once

#include "lab.hpp"

// Lab-only research probes for the Peace Walker engine (Phase 0 of docs/pw/NEXT-SESSION.md).
// Each is a lab key in MGSPWEnabler.lab.settings; a Release build has none of this.
namespace Probe
{
    // Logs the modules loaded at init and again a few seconds later: which of d3d11.dll,
    // d3d12.dll, dxgi.dll, D3D12Core.dll the game actually loads.
    MGS4E_LAB_SWITCH(bool, bLogLoadedModules, true);

    // Polls the first page of .text from init and logs when its bytes stop looking random:
    // how long "wait for the DRM to decrypt the code" must be before pattern scans.
    MGS4E_LAB_SWITCH(bool, bLogDecryptTiming, true);

    // Logs the process command line (the launcher's -resolution/-upscale/-movie arguments).
    MGS4E_LAB_SWITCH(bool, bLogCommandLine, true);

#if MGS4E_LAB_BUILD
    void Run();
#else
    inline void Run() {}
#endif
}
