#pragma once

// WITHDRAWN 2026-09-19, the same day it was written: it broke the game for the user and the dead
// zones did not work. NOT in the project file, NOT installed, kept only so the next attempt starts
// from the faults rather than repeating them. docs/pw/mouse-input.md 2n has the account.
//
// THREE KNOWN DEFECTS, all in this file:
//
//  1. The two dead-zone hooks could never work. A safetyhook mid-hook runs the callback and THEN
//     the original instruction, so setting xmm0 at +73EA7E is immediately overwritten by the very
//     `movss xmm0, [48.0]` that was hooked, and the same for xmm1 at +73EAC3. The codebase pattern
//     for replacing what a load produces is internal_size.cpp's: set the register AND advance
//     `ctx.rip` past the load (there `ctx.rip += 5`; these loads are 8 bytes). The alternative is
//     to hook the four consumers instead, where the value is read rather than written.
//  2. The late sample corrupts the input source. It recomputes the four look actions but leaves
//     their source-kind fields (+0x1C / +0x20) as the game's own update left them, and the look
//     routine derives player+0x9B2 ("input is mouse") from exactly those fields at +73E92B. A
//     mouse frame can therefore be taken for a pad frame, which sends the mouse through the pad's
//     48-unit dead zone and silences it, since a mouse look value is well under one unit.
//  3. Re-entering SteamInputWork::Update from inside the actor scheduler was never established to
//     be safe: it re-reads sixteen digital actions as well as the two sticks, and nothing here
//     checks what that does to button edges.
//
// Before trying again: fix 1 mechanically, then prove 2 with a Lab run that logs the source kinds
// and player+0x9B2 with the late sample on, and settle 3 by polling only the analog half.


#include <cstdint>

// Controller (analog stick) input, docs/pw/mouse-input.md 2m. The pad reaches the game through
// Steam Input only: Input::SteamInputWork polls it once a frame from the input manager's update,
// which runs BEFORE the actor scheduler whose wait precedes the player's actor. So the stick, like
// the mouse before it, is a frame old by the time the camera uses it. The game applies no
// sensitivity setting of any kind to pad input, and its dead zones are large.
//
//   bLateSample      Re-poll Steam Input and recompute the four look actions right before the
//                    look-input routine reads them. A stick is a state, not a sum, so there is
//                    nothing to double-count. Off by default until the user has checked that
//                    re-running the poll does not disturb button edges.
//
//   iLookDeadZone    The aim dead zone, in stick units of 127. The game's is 48 (38% of the
//                    stick's travel does nothing). The gain that follows it is rescaled so that
//                    full deflection still produces the same output as the game's.
//
//   iMoveDeadZone    The movement dead zone, same units. The game's band is a per-axis AND over
//                    [-47, +48), so a diagonal at 47/47 (52% of the stick) is discarded whole.
//                    Only lowering is offered: the keyboard's WASD arrives at exactly 48.31 units,
//                    so a larger band would silence it.
namespace StickInput
{
    inline bool bLateSample = false;
    inline int iLookDeadZone = 48;   // the game's value; the slider only goes down
    inline int iMoveDeadZone = 47;

    // What the last frame did, for the Lab probe.
    struct Sample
    {
        uint64_t serial = 0;
        bool polled = false;         // Steam Input was re-polled this frame
        int actionsChanged = 0;      // look actions whose value the re-poll moved
        float before[4] = {};        // X+, X-, Y+, Y- as they stood
        float after[4] = {};         // and after the re-poll
        int64_t ticks = 0;
    };
    const Sample& Last();

    // Called from the look-input hook (+73E78A), before the routine reads its four actions and
    // before the mouse's own late sample folds the accumulated mouse movement in.
    void OnLookInput();

    void Install();
}
