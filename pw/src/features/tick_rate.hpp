#pragma once
#include "lab.hpp"

// EXPERIMENT, Lab only. The game's frame limiter (the ticker at +76160..+76210) holds its target
// period as a double of 1/60 loaded into xmm7 at +76188, then spins: read QPC, and while
// 1/60 > elapsed, Sleep the remainder. Changing that one value changes how often the whole game
// loop runs.
//
// The simulation is FIXED-STEP: the user confirms game speed follows the frame rate, and the one
// delta the engine does compute is (1 + dropped frames) / 60 with the 60 baked in at the site. So
// raising the rate WILL run the game fast. That is the point of this switch: the failure pattern
// at a higher rate says whether a handful of systems need scaling or the whole engine does, which
// no amount of static reading settles as cheaply.
//
// Peace Walker ran at 20 fps on the PSP and its console port at 60, and sibling titles on this
// engine family have had their rate raised, so a rate mechanism probably exists somewhere; this
// switch is how we find out what it does not cover.
//
// Never ship this on. It is deliberately absent from a Release build.
namespace TickRate
{
    MGS4E_LAB_SWITCH(int, iRate, 60);   // 60 = the game's own; the hook does nothing at 60

    void Install();
}
