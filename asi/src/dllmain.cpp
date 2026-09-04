#include "pch.hpp"

#include "core/config.hpp"
#include "core/game.hpp"
#include "core/log.hpp"
#include "core/upstream.hpp"
#include "version.hpp"

#include "features/render_pipeline.hpp"
#include "features/stage_automation.hpp"

namespace
{
    HANDLE g_InitMutex = nullptr;
    bool g_SecondCopy = false;

    // One copy of PF Companion per process. A second copy (a stray .asi in another folder the
    // loader also scans) would install every hook twice; it sees the mutex and stays inert.
    bool AnotherCopyIsLoaded()
    {
        const std::wstring name = std::format(L"Local\\{}_Init_{}", L"" PFC_NAME, GetCurrentProcessId());
        g_InitMutex = CreateMutexW(nullptr, FALSE, name.c_str());
        return g_InitMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS;
    }

    void Initialize()
    {
        static std::once_flag once;
        std::call_once(once, []
        {
            pfc::game::Detect();
            pfc::log::Initialize();

            if (!pfc::game::IsMgs4())
            {
                spdlog::warn("Not mgs4.exe - nothing to do.");
                return;
            }

            pfc::config::Load();
            pfc::upstream::Detect();

            // Render pipeline first: it resolves the render size globals the aspect fixes
            // read, and calls GraphicsSettings and AspectRatio itself in the order they need.
            RenderPipeline::ApplyFixes();
            StageAutomation::ApplyFixes();

            spdlog::info("Initialisation complete.");
        });
    }
}

// Ultimate ASI Loader calls this right after DllMain returns, in load order, so the
// neighbour's hooks are already in place when this runs.
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
