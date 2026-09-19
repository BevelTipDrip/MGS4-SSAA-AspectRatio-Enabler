#include "pch.hpp"
#include "busy_wait.hpp"

#include "game.hpp"
#include "log.hpp"
#include "sites.hpp"

#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

namespace
{
    // The three sites, found by signature (sites.hpp) and then checked against their own bytes
    // before anything is hooked; a mismatch installs nothing.
    constexpr mgspwe::sites::Signature kFrameReadyStoreSig { "busy wait: frame ready store", 0x1768A, "44 89 A3 6C 2A 00 00 44 89 A3 B4 2A 00 00", 0 };   // immediately after mov byte [rbx+0x32c0], 1
    constexpr mgspwe::sites::Signature kRenderSpinSig      { "busy wait: render spin", 0x17CCF, "33 C9 FF 15 ?? ?? ?? ?? 40 38 AF C8 3B 00 00", 0 };   // xor ecx, ecx ; call Sleep
    constexpr mgspwe::sites::Signature kTickerSleepSig     { "busy wait: ticker sleep", 0x761D1, "B9 01 00 00 00 FF 15 ?? ?? ?? ?? 0F 28 C7", 0 };   // mov ecx, 1 ; call timeBeginPeriod
    uintptr_t kFrameReadyStore = 0, kRenderSpin = 0, kTickerSleep = 0;

    // Load-bearing for shutdown. The render thread's loop only re-reads the quit flag at
    // display+0x3bc8 after its wait returns, so this timeout is what guarantees it wakes when the
    // final frame never comes. It is also what makes a missed signal cost one timeout and then
    // behave exactly as the unfixed game does, rather than hanging. Do not remove it.
    constexpr DWORD kHandoffTimeoutMs = 2;

    constexpr double kTickSpinMarginMs = 1.0;   // left for the game's own loop to spin out
    constexpr double kTickMaxWaitMs = 50.0;     // a bad clock read must not park the game

    std::atomic<int> g_Level { 0 };
    HANDLE g_FrameReady = nullptr;
    HANDLE g_Timer = nullptr;
    bool g_PeriodHeld = false;   // timeBeginPeriod(1) for the life of the process, as MGSHDFix does

    // The process timer resolution the kernel is actually granting, in milliseconds.
    double TimerResolutionMs()
    {
        using Fn = LONG(NTAPI*)(PULONG, PULONG, PULONG);
        static const auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryTimerResolution"));
        ULONG mn = 0, mx = 0, cur = 0;
        if (!fn || fn(&mn, &mx, &cur) != 0) { return 0; }
        return cur / 10000.0;
    }
    safetyhook::MidHook g_HandoffSignal, g_RenderSpin, g_TickerSpin;
    safetyhook::InlineHook g_PeekMessage;
    safetyhook::MidHook g_PumpSleep;
    using PeekMessageFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);

    std::atomic<uint64_t> g_FrameWaits { 0 }, g_FrameWaitTimeouts { 0 }, g_FrameWaitTicks { 0 };
    std::atomic<uint64_t> g_TickSleeps { 0 }, g_TickTicks { 0 };
    std::atomic<uint64_t> g_PumpWaits { 0 };
    std::atomic<int64_t> g_Report { 0 };

    int64_t Ticks() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
    double TicksToMs(int64_t t)
    {
        static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return 1000.0 / static_cast<double>(q.QuadPart); }();
        return static_cast<double>(t) * f;
    }

    void PreciseSleep(double ms)
    {
        if (ms <= 0 || !g_Timer) { return; }
        LARGE_INTEGER due {};
        due.QuadPart = -static_cast<LONGLONG>(ms * 10000.0);   // relative, 100 ns units
        if (SetWaitableTimer(g_Timer, &due, 0, nullptr, nullptr, FALSE))
        {
            WaitForSingleObject(g_Timer, static_cast<DWORD>(ms) + 2);
        }
    }

    void Report()
    {
        const int64_t now = Ticks();
        int64_t last = g_Report.load();
        if (!last) { g_Report.store(now); return; }
        if (TicksToMs(now - last) < 5000.0) { return; }
        if (!g_Report.compare_exchange_strong(last, now)) { return; }
        const uint64_t w = g_FrameWaits.exchange(0), to = g_FrameWaitTimeouts.exchange(0), wt = g_FrameWaitTicks.exchange(0);
        const uint64_t s = g_TickSleeps.exchange(0), st = g_TickTicks.exchange(0);
        spdlog::info("PW busy wait: level {}; handoff waits {} ({:.2f} ms each, {} timed out), ticker sleeps {} ({:.2f} ms each), pump waits {}; timer resolution {:.2f} ms{}.",
            g_Level.load(), w, w ? TicksToMs(static_cast<int64_t>(wt)) / w : 0.0, to,
            s, s ? TicksToMs(static_cast<int64_t>(st)) / s : 0.0, g_PumpWaits.exchange(0), TimerResolutionMs(), g_PeriodHeld ? " (held at 1 ms)" : "");
    }

    // A Sleep(1) cannot be woken by an arriving message, so the window stays unresponsive for the
    // rest of the millisecond and the thread wakes whether or not anything came. This returns the
    // moment a message lands and otherwise costs the same millisecond. MGSHDFix's fix for MGS2 and
    // MGS3 verbatim, including the timeout.
    BOOL WINAPI Hooked_PeekMessageA(LPMSG msg, HWND wnd, UINT first, UINT last, UINT remove)
    {
        BOOL got = g_PeekMessage.call<BOOL>(msg, wnd, first, last, remove);
        if (!got && g_Level.load(std::memory_order_relaxed) >= 3)
        {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT);
            g_PumpWaits.fetch_add(1);
            // What ended the wait is handled now rather than one Sleep(1) later.
            if (BusyWait::bPromptPump) { got = g_PeekMessage.call<BOOL>(msg, wnd, first, last, remove); }
        }
        return got;
    }
}

namespace BusyWait
{
    bool Installed() { return static_cast<bool>(g_RenderSpin); }
    bool PumpHooked() { return static_cast<bool>(g_PeekMessage); }

    void SetLevel(int level)
    {
        g_Level.store(std::max(0, std::min(3, level)));
        // Never leave the render loop parked on a wait that the old behaviour would not have made.
        if (g_Level.load() >= 1 && g_FrameReady) { SetEvent(g_FrameReady); }
    }

    void Install()
    {
        if (iLevel <= 0) { return; }

        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        kFrameReadyStore = mgspwe::sites::Resolve(kFrameReadyStoreSig);
        kRenderSpin = mgspwe::sites::Resolve(kRenderSpinSig);
        kTickerSleep = mgspwe::sites::Resolve(kTickerSleepSig);
        if (!kFrameReadyStore || !kRenderSpin || !kTickerSleep) { spdlog::warn("PW busy wait: a site was not found in this build; nothing hooked."); return; }
        const auto* store = reinterpret_cast<const uint8_t*>(base + kFrameReadyStore - 7);
        const auto* spin = reinterpret_cast<const uint8_t*>(base + kRenderSpin);
        const auto* tick = reinterpret_cast<const uint8_t*>(base + kTickerSleep);
        // mov byte [rbx+0x32c0], 1 / xor ecx, ecx ; call [rip+x] / mov ecx, 1 ; call [rip+x].
        // The xor is encoded 33 C9, not 31 C9: read the bytes, never assume the encoding.
        if (!(store[0] == 0xC6 && store[1] == 0x83 && store[6] == 0x01
              && spin[0] == 0x33 && spin[1] == 0xC9 && spin[2] == 0xFF
              && tick[0] == 0xB9 && tick[1] == 0x01 && tick[5] == 0xFF))
        {
            spdlog::warn("PW busy wait: the sites +{:X}/+{:X}/+{:X} do not look as expected; nothing hooked.", kFrameReadyStore, kRenderSpin, kTickerSleep);
            return;
        }

        g_FrameReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
        g_Timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        // The game only asks for 1 ms timer resolution around its own ticker sleep, which the
        // ticker hook turns into a few microseconds a tick; every other timed wait in the process
        // (the driver's polling inside Present among them) then runs at the default ~15.6 ms. Hold
        // the resolution for the whole run instead, as MGSHDFix does for MGS2/3 (busy_loop_fix.cpp).
        if (iLevel >= 2 && timeBeginPeriod(1) == TIMERR_NOERROR) { g_PeriodHeld = true; }
        if (!g_FrameReady)
        {
            spdlog::warn("PW busy wait: no event could be created; the render thread keeps spinning.");
            return;
        }

        // The game thread has just published the frame: wake the render thread. Hooked after the
        // store, so the flag the loop re-reads is already set when it wakes.
        g_HandoffSignal = safetyhook::create_mid(base + kFrameReadyStore, [](SafetyHookContext&)
        {
            if (g_Level.load(std::memory_order_relaxed) >= 1) { SetEvent(g_FrameReady); }
        });

        // The render thread has no frame: wait for one instead of spinning on Sleep(0). We wait on
        // the game thread, not on a clock, which is why this signals rather than sleeping blind the
        // way the ticker does; a blind sleep here would only delay the frame.
        g_RenderSpin = safetyhook::create_mid(base + kRenderSpin, [](SafetyHookContext&)
        {
            if (g_Level.load(std::memory_order_relaxed) < 1) { return; }
            const int64_t t0 = Ticks();
            const DWORD r = WaitForSingleObject(g_FrameReady, kHandoffTimeoutMs);
            g_FrameWaitTicks.fetch_add(static_cast<uint64_t>(Ticks() - t0));
            g_FrameWaits.fetch_add(1);
            if (r == WAIT_TIMEOUT) { g_FrameWaitTimeouts.fetch_add(1); }
            Report();
        });

        // The tick is not due: sleep the time left on a high resolution timer, less a margin, and
        // neutralise the game's own sleep by telling it no time remains. xmm6 is recomputed from the
        // clock at the top of every iteration, so clobbering it is safe.
        g_TickerSpin = safetyhook::create_mid(base + kTickerSleep, [](SafetyHookContext& ctx)
        {
            if (g_Level.load(std::memory_order_relaxed) < 2 || !g_Timer) { return; }
            const double remainingMs = (ctx.xmm7.f64[0] - ctx.xmm6.f64[0]) * 1000.0;
            if (!std::isfinite(remainingMs) || remainingMs <= kTickSpinMarginMs) { return; }
            const int64_t t0 = Ticks();
            PreciseSleep(std::min(remainingMs - kTickSpinMarginMs, kTickMaxWaitMs));
            g_TickTicks.fetch_add(static_cast<uint64_t>(Ticks() - t0));
            g_TickSleeps.fetch_add(1);
            ctx.xmm6.f64[0] = ctx.xmm7.f64[0];
        });

        if (bPromptPump || MGS4E_LAB_BUILD)   // in a Lab build the switch is live (F8), so the hook has to be there
        {
            // The pump loop's Sleep(1): call [Sleep] with ecx = 1, then the jump back to PeekMessageA.
            constexpr mgspwe::sites::Signature kPumpSleepSig { "busy wait: pump Sleep(1)", 0x777E2, "FF 15 ?? ?? ?? ?? EB A6 44 38 25 ?? ?? ?? ??", 0 };
            if (const uintptr_t site = mgspwe::sites::Resolve(kPumpSleepSig))
            {
                g_PumpSleep = safetyhook::create_mid(base + site, [](SafetyHookContext& ctx)
                {
                    if (BusyWait::bPromptPump && g_PeekMessage && g_Level.load(std::memory_order_relaxed) >= 3) { ctx.rcx = 0; }
                });
                spdlog::info("PW busy wait: prompt pump {}: pump sleep +{:X} {}.", bPromptPump ? "ON" : "off", site, g_PumpSleep ? "hooked" : "FAILED");
            }
        }

        // The pump lives in user32, so the hook goes there rather than on an import: the game
        // imports PeekMessageA, but so do the overlays, and only the level gate decides who waits.
        if (const HMODULE user32 = GetModuleHandleW(L"user32.dll"))
        {
            if (const auto peek = reinterpret_cast<PeekMessageFn>(GetProcAddress(user32, "PeekMessageA")))
            {
                g_PeekMessage = safetyhook::create_inline(reinterpret_cast<void*>(peek), reinterpret_cast<void*>(Hooked_PeekMessageA));
            }
        }

        // Neither mid hook redirects execution, so nothing depends on safetyhook's rip semantics:
        // the render thread's Sleep(0) runs after our wait as a cheap yield, and the ticker's own
        // sleep computes zero. Keep it that way.
        g_Level.store(iLevel);
        spdlog::info("PW busy wait: level {}; handoff signal +{:X} {}, render spin +{:X} {}, ticker spin +{:X} {}; {} timer; message pump {}.",
            iLevel, kFrameReadyStore, g_HandoffSignal ? "hooked" : "FAILED",
            kRenderSpin, g_RenderSpin ? "hooked" : "FAILED",
            kTickerSleep, g_TickerSpin ? "hooked" : "FAILED",
            g_Timer ? "high resolution" : "no",
            g_PeekMessage ? "hooked at PeekMessageA" : "NOT hooked");
    }
}
