#pragma once

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
