#pragma once

#include "lab.hpp"

// Read polls (Lab switch while it is tested): the save list's 70 ms menu stall.
//
// The game reads through an emulated disc layer and waits for a request by polling it from
// the game thread: `while (busy(request)) sleep(16683 us)` - a whole 60 Hz frame between two
// checks. Loading one save entry into the list takes several such requests in a row, so every
// entry that scrolls into view costs about four frames even when the data is read at once
// (measured with the stall sampler: 68.6-70.8 ms per entry, no other thread working).
//
// The fix keeps each loop as it is and shortens only the wait inside it: the request is
// checked about every millisecond and the wait ends as soon as it is done, or after the full
// frame as before. A loop therefore never runs more often than it did (a counter the game
// bumps per pass cannot advance faster), it only stops waiting for nothing. The busy check is
// the game's own, called exactly as the loop calls it.
namespace ReadPolls
{
    MGS4E_LAB_SWITCH(bool, bEnabled, false);

    void Install();
}
