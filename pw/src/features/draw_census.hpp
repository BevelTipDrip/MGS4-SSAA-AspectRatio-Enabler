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
    MGS4E_LAB_SWITCH(int, iTimeFrames, 60);       // frames to time (one every four) and average
    MGS4E_LAB_SWITCH(bool, bDumpShaders, false);
    MGS4E_LAB_SWITCH(bool, bPacingLog, false);
    MGS4E_LAB_SWITCH(bool, bHitchSampler, false);
    MGS4E_LAB_SWITCH(int, iVBlankWaitTimeout, 0);
    MGS4E_LAB_SWITCH(int, iFrameHandoffWait, 0);
    MGS4E_LAB_SWITCH(bool, bVBlankWaitLog, false);
    MGS4E_LAB_SWITCH(bool, bFrameSkipGovernor, true); // the game's own: a frame that measures longer than its vblank count raises the count (a skipped frame next); false leaves the count as set // the game thread's vblank waits: ticks passed between waits and the work time in between; every skipped tick is logged with both   // ms; the game drops a finished frame when the render thread still holds the previous one (in a vsync-blocked Present); this waits up to N ms for it instead  // ms; 0 the game's infinite wait on its vblank event (a lost wakeup costs a whole frame); 1 recovers within a millisecond // the render thread's Sleep(0) spin is watched (light import hook); after 20 ms of waiting for a frame every other thread is sampled once and logged    // the pacing instrumentation: import hooks on Sleep/WaitForSingleObject (a hook on every one of the render thread's ~170k Sleep(0) calls a frame: measurable overhead), tick counter, 5 s summaries
    MGS4E_LAB_SWITCH(int, iFramePacing, 0);       // 0 the game's 60.000 Hz ticker; 1 paced to the display's real refresh rate; 2 locked to the display's vblank
    MGS4E_LAB_SWITCH(int, iPresentSync, -1);
    MGS4E_LAB_SWITCH(bool, bAllowTearing, false);  // flip chain created with ALLOW_TEARING; windowed Present(0, ALLOW_TEARING): never blocks on the display      // -1 the game's sync interval (1); 0..4 forced       // 0 the game's 60.000 Hz ticker; 1 the ticker paced to the display's real refresh rate
    MGS4E_LAB_SWITCH(bool, bLightHooks, false);   // hook only the resolve on the contexts: no per-draw cost, no census, no timing  // write every shader's bytecode to C:\mgspf_tools\pw\shaders

#if MGS4E_LAB_BUILD
    // Hooks the D3D11 device entry points; the context and swap chain hooks follow when the
    // game creates them. Call at init, before the game creates its device.
    void Install();
#else
    inline void Install() {}
#endif
}
