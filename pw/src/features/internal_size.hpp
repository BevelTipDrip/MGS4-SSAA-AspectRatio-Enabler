#pragma once

#include "lab.hpp"

// Lab-only experiment: the internal render size and the output size, set directly.
//
// The game renders the whole frame (world and HUD) into one internal target whose size is an
// immediate constant chosen by the launcher's -resolution flag (1440x816 or 1920x1088), then
// blits it, through its own fit, into the output size from a table chosen by -upscale. This
// probe rewrites those immediates and the table at init, so the internal target can be any
// size (e.g. twice the window) and the output the window itself: the game's final blit then
// downsamples, with no oversized window. Measured with the census; see the private notes.
namespace InternalSize
{
    MGS4E_LAB_SWITCH(int, iRenderScale, 0);      // integer canvas multiple k (game: 3 or 4); 0: leave alone. Sets the scale getter and, unless given, the internal size 480k x 272k
    MGS4E_LAB_SWITCH(int, iInternalWidth, 0);    // 0: derive from the render scale, or leave the game's immediates alone
    MGS4E_LAB_SWITCH(int, iInternalHeight, 0);
    MGS4E_LAB_SWITCH(int, iOutputWidth, 0);      // 0: leave the output table alone
    MGS4E_LAB_SWITCH(int, iOutputHeight, 0);

#if MGS4E_LAB_BUILD
    void Apply();
#else
    inline void Apply() {}
#endif
}
