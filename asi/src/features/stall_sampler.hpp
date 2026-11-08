#pragma once

#include "lab.hpp"

// Stall sampler (Lab only): where the game's threads spend a long gap between two Presents.
//
// A sampler thread reads the call stack of the game's main thread ("MGS4 MAIN") and of the
// thread that presents every millisecond, and of every other thread in the process every
// 20 ms, into a ring. When two Presents are further apart than the threshold, the samples
// inside that gap are grouped by call chain (function starts from each module's unwind
// table, so no symbols are needed and the addresses can be looked up in the code dumps) and
// logged: the main and presenting threads' commonest chains, and every other thread that
// was running rather than blocked in a system call.
//
// Nothing is allocated, locked or logged while a thread is suspended: the stack is walked
// with RtlVirtualUnwind against unwind tables the sampler indexed itself beforehand, so a
// suspended thread holding the heap or loader lock cannot deadlock the sampler.
namespace StallSampler
{
    MGS4E_LAB_SWITCH(bool, bEnabled, false);
    MGS4E_LAB_SWITCH(int, iThresholdMs, 40);   // a gap between Presents at least this long is a stall
    MGS4E_LAB_SWITCH(int, iBudget, 80);        // stalls logged per session

#if MGS4E_LAB_BUILD
    void Start();
    void OnPresent();   // called from the Present hooks, on the presenting thread
#else
    inline void Start() {}
    inline void OnPresent() {}
#endif
}
