#pragma once
#include "lab.hpp"

// Lab only. Measures how mouse movement reaches the engine, as the first step of the mouse aim
// work (docs/pw/mouse-input.md). The game already reads the mouse through Raw Input:
//
//   +3A390  raw mouse handler      relative moves only; accumX += dx/256, accumY += -dy/256
//   +3A850  per-frame device update copies the accumulators into the device (+0x848/+0x84C), zeroes them
//   +3A9F0  virtual value getter   every input as a float 0..255; the mouse is four directions
//                                  (codes 0x10008 Y+, 0x10009 Y-, 0x1000A X-, 0x1000B X+)
//
// The probe hooks the three, and logs one line per frame that had mouse activity: the raw counts
// that arrived, what was latched, what the getter answered for each direction, all stamped with
// QueryPerformanceCounter so they line up with the harness injector (C:\mgspf_tools\pw\mouse_move.ps1).
// The getter is only ever called through a vtable, so each distinct caller is logged once with a
// short stack: that is the way into the binding layer and, from there, the camera.
namespace InputProbe
{
    MGS4E_LAB_SWITCH(bool, bEnabled, false);

    // Hardware data breakpoints on the camera actions' current values (how the look-input routine
    // was found). Off unless asked for: each access costs an exception.
    MGS4E_LAB_SWITCH(bool, bWatchActions, false);

    void Install();
}
