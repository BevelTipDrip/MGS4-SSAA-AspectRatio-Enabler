#pragma once

// Lab builds and release builds.
//
// The research instrumentation - the probes, scans, memory watches, dumps and hotkeys, the
// lab settings file and lab mode itself - exists only in a Lab build: the Lab configuration,
// which defines MGS4E_LAB. In a release build every one of its switches is a compile-time
// constant `false`, so the `if (switch)` around each piece of instrumentation is dead code.
// The compiler drops the code, the functions only it referenced, and every string they
// carried; the shipped binary holds the fixes and nothing that explains how they were found.
//
// Declare a research switch with MGS4E_LAB_SWITCH instead of `inline`. It is a variable in a
// Lab build (the lab settings file sets it) and a constexpr in a release build (nothing can).
// Code that only a Lab build can reach and that a constant switch cannot remove on its own
// (a block that parses a string setting, say) goes under `#if MGS4E_LAB_BUILD`.
#ifdef MGS4E_LAB
#define MGS4E_LAB_BUILD 1
#define MGS4E_LAB_SWITCH(type, name, value) inline type name = value
#else
#define MGS4E_LAB_BUILD 0
#define MGS4E_LAB_SWITCH(type, name, value) inline constexpr type name = value
#endif
