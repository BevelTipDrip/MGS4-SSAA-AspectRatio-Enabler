#include "pch.hpp"
#include "read_polls.hpp"

#include "game.hpp"
#include "sites.hpp"

#include <atomic>
#include <cstring>

namespace ReadPolls
{
    namespace
    {
        // The three save-list loops. Each pattern starts where the loop does and covers the
        // frame sleep (mov ecx, 0x412B = 16683 us; call sleep_us), the reload of the request
        // and the busy check (call busy; test eax, eax; jnz loop). Unique in BGFX 0.0.3 and 0.0.8.
        enum class Request { Rdi, RdiC8, RbxC8 };
        struct Loop
        {
            mgs4e::sites::Signature sig;
            size_t sleepCall;   // offset of the call to the sleep, from the match
            size_t busyCall;    // offset of the call to the busy check
            Request request;
        };
        constexpr Loop kLoops[] {
            { { "read poll A", 0x38F560, "B9 2B 41 00 00 E8 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 85 C0 75", 0 }, 5, 13, Request::Rdi },
            { { "read poll B", 0x38F6C0, "FF 87 C0 00 00 00 B9 2B 41 00 00 E8 ?? ?? ?? ?? 48 8B 8F C8 00 00 00 E8 ?? ?? ?? ?? 85 C0 75", 0 }, 11, 23, Request::RdiC8 },
            { { "read poll C", 0x38F7C0, "FF 83 C0 00 00 00 B9 2B 41 00 00 E8 ?? ?? ?? ?? 48 8B 8B C8 00 00 00 E8 ?? ?? ?? ?? 85 C0 75", 0 }, 11, 23, Request::RbxC8 },
        };
        constexpr uint32_t kFrameUs = 0x412B;

        using BusyFn = int (*)(void*);
        BusyFn g_Busy = nullptr;
        SafetyHookMid g_Hooks[std::size(kLoops)];

        std::atomic<uint32_t> g_Logged { 0 };
        std::atomic<uint64_t> g_Waits { 0 };

        int64_t g_Frequency = 1;
        int64_t Ticks()
        {
            LARGE_INTEGER t;
            QueryPerformanceCounter(&t);
            return t.QuadPart;
        }

        uintptr_t CallTarget(const uint8_t* call)
        {
            int32_t rel = 0;
            std::memcpy(&rel, call + 1, sizeof(rel));
            return reinterpret_cast<uintptr_t>(call) + 5 + rel;
        }

        void Wait(SafetyHookContext& ctx, Request kind)
        {
            if (static_cast<uint32_t>(ctx.rcx) != kFrameUs) { return; }
            void* request = nullptr;
            switch (kind)
            {
            case Request::Rdi: request = reinterpret_cast<void*>(ctx.rdi); break;
            case Request::RdiC8: request = *reinterpret_cast<void**>(ctx.rdi + 0xC8); break;
            case Request::RbxC8: request = *reinterpret_cast<void**>(ctx.rbx + 0xC8); break;
            }
            if (!request) { return; }

            const int64_t start = Ticks();
            const int64_t limit = start + g_Frequency * kFrameUs / 1000000;
            bool done = false;
            for (;;)
            {
                if (!g_Busy(request)) { done = true; break; }
                if (Ticks() >= limit) { break; }
                Sleep(1);   // the game holds timeBeginPeriod(1): about a millisecond
            }
            ctx.rcx = 1;   // the game's own sleep now returns at once
            g_Waits.fetch_add(1, std::memory_order_relaxed);
            if (g_Logged.fetch_add(1) < 24)
            {
                spdlog::info("Read polls: request {} after {:.2f} ms (the game would have slept 16.68 ms).",
                    done ? "done" : "still busy", 1000.0 * static_cast<double>(Ticks() - start) / static_cast<double>(g_Frequency));
            }
        }
    }

    void Install()
    {
        if (!bEnabled) { return; }
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_Frequency = f.QuadPart;

        uint8_t* sites[std::size(kLoops)] {};
        for (size_t i = 0; i < std::size(kLoops); i++)
        {
            sites[i] = mgs4e::sites::Address(kLoops[i].sig);
            if (!sites[i])
            {
                spdlog::warn("Read polls: {} not found; the fix is off.", kLoops[i].sig.name);
                return;
            }
        }
        // Every loop must call the same busy check.
        const uintptr_t busy = CallTarget(sites[0] + kLoops[0].busyCall);
        for (size_t i = 1; i < std::size(kLoops); i++)
        {
            if (CallTarget(sites[i] + kLoops[i].busyCall) != busy)
            {
                spdlog::warn("Read polls: the loops call different busy checks; the fix is off.");
                return;
            }
        }
        g_Busy = reinterpret_cast<BusyFn>(busy);

        static_assert(std::size(kLoops) == 3);
        g_Hooks[0] = safetyhook::create_mid(sites[0] + kLoops[0].sleepCall, [](SafetyHookContext& c) { Wait(c, Request::Rdi); });
        g_Hooks[1] = safetyhook::create_mid(sites[1] + kLoops[1].sleepCall, [](SafetyHookContext& c) { Wait(c, Request::RdiC8); });
        g_Hooks[2] = safetyhook::create_mid(sites[2] + kLoops[2].sleepCall, [](SafetyHookContext& c) { Wait(c, Request::RbxC8); });
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        spdlog::info("Read polls: {} of 3 save-list loops wait per millisecond instead of per frame (busy check +{:X}).",
            (g_Hooks[0] ? 1 : 0) + (g_Hooks[1] ? 1 : 0) + (g_Hooks[2] ? 1 : 0), busy - base);
    }
}
