#pragma once

// The process the Peace Walker build is loaded into. The generic parts - module handle, exe
// path, install root (the exe's grandparent: <root>\mgspw\<exe>) - are mgs4e::game from the
// shared core; this adds the one check that is this game's.
namespace mgspwe::game
{
    // Whether the running process is METAL GEAR SOLID PEACE WALKER.exe (case-insensitive).
    // Nothing is patched otherwise. Call after mgs4e::game::Detect().
    bool IsPeaceWalker();
}
