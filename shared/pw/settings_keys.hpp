#pragma once

// The keys in MGSPWEnabler.settings, shared by the Peace Walker ASI and the settings tool so
// the two never disagree on spelling. The MGS4 keys live in ../settings_keys.hpp and are a
// different file on purpose: the two games share no engine, so they share no settings.
namespace mgspwe::keys
{
    constexpr const char* Graphics = "Graphics";
    constexpr const char* Debugging = "Debugging";

    // [Debugging]
    constexpr const char* DebugLogging = "Debug Logging";
}
