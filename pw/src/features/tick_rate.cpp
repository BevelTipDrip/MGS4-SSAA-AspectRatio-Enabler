#include "pch.hpp"
#include "tick_rate.hpp"

#include "game.hpp"
#include "sites.hpp"

namespace
{
    // movsd xmm8, [rip+..] -> 1000.0, the instruction AFTER the one that loads the 1/60 period
    // into xmm7. Hooking it rather than the period load is deliberate: a safetyhook mid-hook runs
    // the callback and then the original instruction, so a hook placed on the period load would
    // have its xmm7 overwritten by the very load it hooked (the fault that withdrew the first
    // controller attempt, docs/pw/mouse-input.md 2n). This instruction writes xmm8, so the xmm7
    // the callback sets survives untouched.
    constexpr mgspwe::sites::Signature kPeriodSig { "tick rate: frame period", 0x76190, "F2 44 0F 10 05 ?? ?? ?? ?? 0F 1F 80 00 00 00 00", 0 };

    SafetyHookMid g_Period;
    std::atomic<uint64_t> g_Ticks { 0 };
}

void TickRate::Install()
{
    if (iRate == 60) { return; }
    const int rate = std::clamp(iRate, 15, 360);
    const uintptr_t site = mgspwe::sites::Resolve(kPeriodSig);
    if (site)
    {
        g_Period = safetyhook::create_mid(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + site),
            [](SafetyHookContext& ctx)
            {
                ctx.xmm7.f64[0] = 1.0 / static_cast<double>(std::clamp(TickRate::iRate, 15, 360));
                const uint64_t n = g_Ticks.fetch_add(1) + 1;
                if (n % 600 == 0) { spdlog::info("PW tick rate: {} frame periods at {} Hz.", n, TickRate::iRate); }
            });
    }
    spdlog::warn("PW tick rate: EXPERIMENT, the game loop is being run at {} Hz instead of 60; the simulation is fixed-step, so expect it to run fast. Period hook +{:X} {}.",
        rate, site, g_Period ? "installed" : "FAILED");
}
