#include "pch.hpp"

#include "config.hpp"
#include "pw_game.hpp"
#include "compat.hpp"
#include "game.hpp"
#include "log.hpp"
#include "version.hpp"

#include "aspect_ratio.hpp"
#include "draw_census.hpp"
#include "internal_size.hpp"
#include "probe.hpp"

namespace
{
    HANDLE g_InitMutex = nullptr;
    bool g_SecondCopy = false;

    // One copy per process: a second copy (a stray .asi in another folder the loader also
    // scans) sees the mutex and stays inert.
    bool AnotherCopyIsLoaded()
    {
        const std::wstring name = std::format(L"Local\\{}_Init_{}", L"" MGSPWE_NAME, GetCurrentProcessId());
        g_InitMutex = CreateMutexW(nullptr, FALSE, name.c_str());
        return g_InitMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS;
    }

    void Initialize()
    {
        static std::once_flag once;
        std::call_once(once, []
        {
            mgs4e::game::Detect();
            mgs4e::log::Initialize();

            if (!mgspwe::game::IsPeaceWalker())
            {
                spdlog::warn("Not {} - nothing to do.", MGSPWE_EXE_NAME);
                return;
            }

            mgspwe::config::Load();
            mgspwe::compat::Detect();
            mgspwe::compat::LogOverlaps();

#if MGS4E_LAB_BUILD
            // A Lab build always runs its research features; their keys (from the user's settings
            // file, or the lab file when the marker is present) decide what each one does.
            Probe::Run();
            DrawCensus::Install();
            InternalSize::Apply();
#endif

            AspectRatio::ApplyFixes();

            spdlog::info("Initialisation complete.");
        });
    }
}

// Ultimate ASI Loader calls this right after DllMain returns, in load order.
extern "C" __declspec(dllexport) void InitializeASI()
{
    if (!g_SecondCopy)
    {
        Initialize();
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_SecondCopy = AnotherCopyIsLoaded();
        if (!g_SecondCopy)
        {
            DisableThreadLibraryCalls(module);
        }
    }
    return TRUE;
}
