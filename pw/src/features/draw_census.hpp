#pragma once

#include "lab.hpp"

// Lab-only draw census for Peace Walker's DirectX 11 renderer: the first step of the UI work,
// the same one the MGS4 lab started with. Every draw for a few frames is logged with the
// render target it lands on, the viewport, the shaders (by bytecode hash), the primitive
// count, the latest constants uploaded to the vertex shader's first buffer, and the game-code
// callers that issued it (a stack walk from the draw, kept to frames inside the game image).
// That is what tells UI draws from world draws, and which game function owns each element.
//
// Triggered by the lab key `Draw Census At (s)` (seconds after init) or by the live command
// `census <frames>` in C:\mgspf_tools\pw\live.txt, which the ASI polls while `Live Commands`
// is on. A Release build has none of this.
namespace DrawCensus
{
    MGS4E_LAB_SWITCH(int, iCensusAtSeconds, 0);     // 0: only on command
    MGS4E_LAB_SWITCH(int, iCensusFrames, 2);
    MGS4E_LAB_SWITCH(bool, bLiveCommands, true);
    MGS4E_LAB_SWITCH(bool, bTimePasses, true);    // GPU timestamps around the passes of a census frame

#if MGS4E_LAB_BUILD
    // Hooks the D3D11 device entry points; the context and swap chain hooks follow when the
    // game creates them. Call at init, before the game creates its device.
    void Install();
#else
    inline void Install() {}
#endif
}
