#pragma once
#include "lab.hpp"

// Peace Walker never blocks. Three loops spin where they should wait, and together they cost about
// a full core; the render thread's is also what the 60 Hz stutter was, all along. This is the fix,
// and unlike the census it is a shipping feature, so it is compiled into both builds.
//
//   render thread  +17BD0, sleep at +17CCF  reads the frame handoff byte at display+0x32c0 and,
//                                           when no frame is ready, calls Sleep(0) and reads it
//                                           again, forever.
//   ticker         +76020, sleep at +76051  truncates the time left to whole milliseconds, so the
//                                           last fraction spins; and calls timeBeginPeriod and
//                                           timeEndPeriod, a system-global lock, every iteration.
//   message pump   +77610                   PeekMessageA then Sleep(1), which no arriving message
//                                           can wake.
//
// Measured in one spot with nothing else changed: process CPU 122% of one core down to 24%, and a
// flat 16.6 ms frame time at a solid 60 on an otherwise stock frame loop. docs/pw/frame-pacing.md
// section 5.6 has the full account, including the four approaches that were tried and rejected
// before this one.
namespace BusyWait
{
    // Cumulative, and deliberately ordered by risk so that "turn it down until it works" keeps the
    // part that matters: 1 is entirely our own event plus two hooks in game code and is almost all
    // of the win; 2 is MGSHDFix's approach for MGS2 and MGS3 applied to our ticker; 3 hooks
    // PeekMessageA inside user32, so every message pump in the process goes through it, overlays
    // included, which makes it the most likely source of a compatibility report and the smallest
    // gain. Hence last.
    // Defaults to the full fix, like the two fullscreen fixes do: this is a defect being corrected,
    // not an option being offered, and a user whose settings file predates the key should still get
    // it. Turning it down is the documented response to a compatibility problem.
    inline int iLevel = 3;

    // docs/pw/mouse-input.md 2h. The pump is also where the mouse is read, on its
    // own thread: PeekMessageA, one message handled (plus a bulk read of pending raw input), then
    // Sleep(1), every pass, message or not. With level 3 the hook waits when the queue is empty, but it
    // then returned "no message", so the message that ended the wait sat through one more Sleep(1).
    // Measured with a clean 1000 Hz source: the handler runs in clumps 4 to 8 ms apart. On: the hook
    // fetches the message that woke it, and the loop's Sleep(1) becomes Sleep(0); the hook's own wait
    // keeps the thread from spinning. Only with level 3, since that wait is what makes it safe.
    inline bool bPromptPump = true;

    void Install();

    // For the Lab build's live command: every hook is installed at start-up and only reads the
    // level, so it can be changed while the game runs and both behaviours compared in one session.
    bool Installed();
    bool PumpHooked();
    void SetLevel(int level);
}
