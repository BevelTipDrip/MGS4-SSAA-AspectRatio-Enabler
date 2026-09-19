#pragma once
#include "lab.hpp"

#include <cstdint>

// Mouse aim fixes (docs/pw/mouse-input.md). The game already reads the mouse through Raw Input
// and already turns the camera linearly with the counts. Two things in that chain are wrong, and
// each has a switch here. Lab switches for now: nothing ships until the user has judged it on a
// Release build.
//
//   bFractionCarry   Each frame's turn is truncated to a whole 16-bit angle unit (cvttss2si at
//                    +4042B6 for the free camera's yaw, +885C5E / +885C6F and +8894A3 / +8894B8 in
//                    two other camera routines) and the fraction is thrown away, always toward
//                    "slower". On: the remainder is kept and added to the next frame's turn, per
//                    axis, when the input is the mouse.
//
//   bLateSample      The game latches the mouse, then runs a frame whose wait comes before the
//                    player's look-input routine: 16.3 ms measured from latch to look value, so
//                    the camera always turns by last frame's movement. On: right before that
//                    routine reads its four actions, pending raw input is dispatched and what has
//                    accumulated is folded into them; the accumulators are cleared so nothing is
//                    counted twice.
namespace MouseAim
{
    MGS4E_LAB_SWITCH(bool, bFractionCarry, false);
    MGS4E_LAB_SWITCH(bool, bLateSample, false);

    // The probe wants the numbers whether or not a fix is on.
    MGS4E_LAB_SWITCH(bool, bTelemetry, false);

    // What the last camera update did, for the Lab probe. Written on the game thread.
    struct CameraSample
    {
        uint64_t serial = 0;        // counts camera updates; the probe compares it frame to frame
        int routine = 0;            // 3 = free camera yaw (+4041D0), 1 = +885360, 2 = +888D77
        bool mouse = false;
        float velocityPitch = 0, velocityYaw = 0;   // what the game computed, angle units for this frame
        int addedPitch = 0, addedYaw = 0;           // what is added to the angles (after the carry, if on)
        float carryPitch = 0, carryYaw = 0;
        uint16_t pitchBefore = 0, yawBefore = 0;
        int64_t qpc = 0;
    };
    const CameraSample& LastCamera();

    // What the last late sample found.
    struct LateSample
    {
        uint64_t serial = 0;
        int messages = 0;           // raw input messages dispatched
        float x = 0, y = 0;         // accumulated since the latch, in the game's units (counts / 256)
        bool applied = false;
        int64_t ticks = 0;          // QPC ticks the whole thing took
    };
    const LateSample& LastLate();

    void Install();
}
