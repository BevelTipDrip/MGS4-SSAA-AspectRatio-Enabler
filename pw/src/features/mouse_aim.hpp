#pragma once
#include "lab.hpp"

#include <cstdint>

// Mouse aim fixes (docs/pw/mouse-input.md). The game already reads the mouse through Raw Input
// and already turns the camera linearly with the counts. Two things in that chain are wrong, and
// each has a switch here. Settings in both builds since 2026-09-19 (Config Tool, Performance), after
// the user judged each one live in the Lab build.
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
    inline bool bFractionCarry = true;
    inline bool bLateSample = true;

    //   bUniformRail     In the free camera, vertical movement is not a rotation: it slides the
    //                    camera along a rail between four presets (distance, height, look-at height
    //                    at +F8F8B8), so the view's elevation changes by 0.011 to 0.038 degrees a
    //                    count depending on where the camera is, with a jump of three times at
    //                    each preset, against a constant 0.0239 horizontally (docs/pw/mouse-input.md
    //                    2j). On: the step along the rail is chosen so that one count changes the
    //                    view's elevation by fVerticalRatio times what it turns the yaw, everywhere
    //                    on the rail. The game's own clamp of the rail position still applies.
    inline bool bUniformRail = false;   // changes how the free camera's vertical axis feels: the user's choice, off until asked for
    // Vertical mouse sensitivity as a multiple of the horizontal, in every camera: the aimed cameras'
    // pitch turn, the free camera's rail step, and the uniform rail's target. Mouse input only.
    inline float fVerticalRatio = 1.0f;

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

    // The free camera's rail this frame: where it was, the step the game wanted, the step taken,
    // and what the pose routine then interpolated from the presets.
    struct RailSample
    {
        uint64_t serial = 0, poseSerial = 0;
        bool mouse = false, adjusted = false;
        float position = 0, minimum = 0, maximum = 0, speed = 0;   // cam + 0x374, + 0xB8, + 0xBC, + 0xA0
        float stepGame = 0, stepTaken = 0;                         // change of the rail position the game computed / the one applied
        float elevationBefore = 0, elevationTarget = 0;            // degrees, from the presets
        float poseDistance = 0, poseHeight = 0, poseLookAt = 0;    // what the pose routine interpolated
    };
    const RailSample& LastRail();

    void Install();
}
