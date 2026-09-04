#pragma once

#include <spdlog/spdlog.h>

// Logging goes to <root>\logs\MGS4Enabler_Game.log, truncated on every launch.
namespace mgs4e::log
{
    // Opens the log file and installs it as spdlog's default logger. Falls back to a
    // null logger if the file cannot be created, so callers never have to check.
    void Initialize();

    // Whether verbose ("Debug Logging") output is wanted. Off until the settings file says
    // otherwise; hooks and scans report at info level only when this is on.
    bool Verbose();
    void SetVerbose(bool verbose);

    // Flushes and closes the file.
    void Shutdown();
}

// Reports whether a safetyhook hook (or anything convertible to bool) was installed.
// A plain block, so it reads as a statement with or without a trailing semicolon.
#define MGS4E_LOG_HOOK(hook, tag)                                        \
    {                                                                  \
        if (hook)                                                      \
        {                                                              \
            if (mgs4e::log::Verbose())                                   \
            {                                                          \
                spdlog::info("{}: hook installed.", tag);              \
            }                                                          \
        }                                                              \
        else                                                           \
        {                                                              \
            spdlog::error("{}: hook failed.", tag);                    \
        }                                                              \
    }
