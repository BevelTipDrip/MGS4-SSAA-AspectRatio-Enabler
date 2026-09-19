#include "pch.hpp"
#include "vrr_compat.hpp"
#include "nv_profile.hpp"
#include "game.hpp"
#include "version.hpp"

#include <thread>

namespace
{
    using NvBool = uint8_t;

    struct GetSleepStatusParams   // NV_GET_SLEEP_STATUS_PARAMS_V1 (nvapi.h, pack 8)
    {
        uint32_t version;
        NvBool lowLatencyMode;
        NvBool fsVrr;
        NvBool cplVsyncOn;
        uint32_t sleepIntervalUs;
        NvBool useGameSleep;
        NvBool fullscreenIFlip;
        uint8_t fgMultiplier;
        NvBool dfgControl;
        uint32_t dfgFrameTimeTargetUs;
        uint8_t reserved[114];
    };
    static_assert(sizeof(GetSleepStatusParams) == 136);

    constexpr uint32_t kIdGetSleepStatus = 0xAEF96CA1;   // NvAPI_D3D_GetSleepStatus

    using GetSleepStatusFn = int (*)(IUnknown*, GetSleepStatusParams*);
    GetSleepStatusFn g_GetSleepStatus = nullptr;

    void Report(ID3D11Device* device, const char* when)
    {
        GetSleepStatusParams s {};
        s.version = sizeof(GetSleepStatusParams) | (1u << 16);
        const int r = g_GetSleepStatus(device, &s);
        if (r != 0) { spdlog::info("PW vrr: {}: sleep status query returned {}.", when, r); return; }
        spdlog::info("PW vrr: {}: low latency mode {}, VRR {}, control panel V-Sync override {}, latest driver sleep interval {} us{}, fullscreen or independent flip {}, game calls NvAPI sleep {}.",
            when, s.lowLatencyMode ? "ON" : "off", s.fsVrr ? "ACTIVE" : "not active", s.cplVsyncOn ? "ON" : "off",
            s.sleepIntervalUs, s.sleepIntervalUs ? fmt::format(" ({:.2f} per second)", 1000000.0 / s.sleepIntervalUs) : std::string(),
            s.fullscreenIFlip ? "yes" : "no", s.useGameSleep ? "yes" : "no");
    }
}

void VrrCompat::ApplyProfile()
{
    // Off with no state file returns inside FastSync without loading nvapi64.dll.
    const std::filesystem::path state = mgs4e::game::Root() / (std::string(MGSPWE_NAME) + ".nvidia.state");
    const NvProfile::Outcome outcome = NvProfile::FastSync(bFastSync, mgs4e::game::ExePath(), state);
    for (const std::string& line : outcome.lines) { spdlog::info("PW vrr: NVIDIA Fast Sync {}: {}", bFastSync ? "on" : "off", line); }
}

void VrrCompat::OnDevice(ID3D11Device* device)
{
    if ((!bFastSync && !bReport) || !device) { return; }
    static bool done = false;
    if (done) { return; }
    done = true;
    g_GetSleepStatus = reinterpret_cast<GetSleepStatusFn>(NvProfile::Interface(kIdGetSleepStatus));
    if (!g_GetSleepStatus) { return; }   // not an NVIDIA system, or a driver without the query

    Report(device, "at device creation");

    // The driver arms its pacing once the game is presenting and variable refresh has engaged,
    // which was measured tens of seconds after device creation, so the state that matters is the
    // one during play. A detached thread that sleeps; nothing per frame.
    device->AddRef();
    std::thread([device]
    {
        const int last = VrrCompat::bReport ? 180 : 46;
        for (int second = 2; second <= last; second += 2)
        {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (second == 46 || second == 106 || second == 180) { Report(device, fmt::format("{} s in", second).c_str()); }
        }
        device->Release();
    }).detach();
}
