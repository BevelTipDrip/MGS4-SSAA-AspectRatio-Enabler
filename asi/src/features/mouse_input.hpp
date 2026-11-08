#pragma once

#include "lab.hpp"

// Mouse input thread (Lab switch while it is tested): high polling-rate mice without stalls.
//
// The game's window is GLFW 3.3. Its glfwPollEvents drains every message on the game thread
// ("MGS4 MAIN") each frame, and every mouse message there makes several win32k calls
// (PeekMessage, GetPropW, SetCursor, ...). With an 8000 Hz mouse win32k is busy with 8000
// reports a second and each of those calls gets far slower: fast camera turns stalled the game
// thread for 40-120 ms inside the pump (measured with the stall sampler, 2026-11-08; gone at
// 1000 Hz). MGS2 had the same mechanism (MGS2-Lab, mouse-input.md 3.11).
//
// Here the mouse is read by a thread of our own instead: a message-only window registered for
// Raw Input with RIDEV_NOLEGACY, so Windows sends the game's window no mouse messages at all,
// drained at most once a millisecond. Once a frame, at the start of glfwPollEvents on the game
// thread, the totals are handed to GLFW exactly as its window procedure would hand them: the
// summed counts onto the virtual cursor in camera mode (cursor disabled), the cursor's client
// position otherwise, then each button change and the wheel. Other Raw Input registrations of
// the mouse (GLFW's raw motion mode, which other mods switch on) are kept from replacing ours.
namespace MouseInput
{
    MGS4E_LAB_SWITCH(bool, bEnabled, false);

    void Install();
}
