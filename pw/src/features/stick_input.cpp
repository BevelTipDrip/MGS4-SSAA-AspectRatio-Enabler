#include "pch.hpp"

// WITHDRAWN 2026-09-19, the same day it was written: it broke the game for the user and the dead
// zones did not work. NOT in the project file, NOT installed, kept only so the next attempt starts
// from the faults rather than repeating them. docs/pw/mouse-input.md 2n has the account.
//
// THREE KNOWN DEFECTS, all in this file:
//
//  1. The two dead-zone hooks could never work. A safetyhook mid-hook runs the callback and THEN
//     the original instruction, so setting xmm0 at +73EA7E is immediately overwritten by the very
//     `movss xmm0, [48.0]` that was hooked, and the same for xmm1 at +73EAC3. The codebase pattern
//     for replacing what a load produces is internal_size.cpp's: set the register AND advance
//     `ctx.rip` past the load (there `ctx.rip += 5`; these loads are 8 bytes). The alternative is
//     to hook the four consumers instead, where the value is read rather than written.
//  2. The late sample corrupts the input source. It recomputes the four look actions but leaves
//     their source-kind fields (+0x1C / +0x20) as the game's own update left them, and the look
//     routine derives player+0x9B2 ("input is mouse") from exactly those fields at +73E92B. A
//     mouse frame can therefore be taken for a pad frame, which sends the mouse through the pad's
//     48-unit dead zone and silences it, since a mouse look value is well under one unit.
//  3. Re-entering SteamInputWork::Update from inside the actor scheduler was never established to
//     be safe: it re-reads sixteen digital actions as well as the two sticks, and nothing here
//     checks what that does to button edges.
//
// Before trying again: fix 1 mechanically, then prove 2 with a Lab run that logs the source kinds
// and player+0x9B2 with the late sample on, and settle 3 by polling only the analog half.

#include "stick_input.hpp"

#include "game.hpp"
#include "sites.hpp"

namespace
{
    // Signatures made with the harness's sig_one.py; each is unique in the 2026-09-18 build and in
    // the one before it. Never patch the constants themselves: every one of them is shared with
    // unrelated code, so these hooks rewrite the register the loading instruction just filled.

    // movss xmm0, [rip+..] -> 48.0: the look dead zone, loaded ONCE and consumed by the four
    // mutually exclusive branches that follow (X > 0 subtract, X < 0 add, and the same for Y).
    constexpr mgspwe::sites::Signature kLookDeadZoneSig { "stick: look dead zone load", 0x73EA7E, "F3 0F 10 05 ?? ?? ?? ?? 76 19 F3 44 0F 5C C0", 0 };
    // movss xmm1, [rip+..] -> 1.6: the gain applied after the dead zone, to both axes.
    constexpr mgspwe::sites::Signature kLookGainSig { "stick: look gain load", 0x73EAC3, "F3 0F 10 0D ?? ?? ?? ?? F3 44 0F 59 C1", 0 };
    // cvttss2si eax, xmm1 at the movement band gate: xmm1 = one axis + 128.0, ecx = the other,
    // already truncated. Both are dead after the compares, which test the fixed byte band
    // [0x51, 0xAF] with sub al, 0x51 / cmp al, 0x5E.
    constexpr mgspwe::sites::Signature kMoveBandSig { "stick: movement dead zone band", 0x73E5D5, "F3 0F 2C C1 2C 51 3C 5E 77 0C 80 E9 51", 0 };
    // mov rcx, [rip+..] (the Input::SteamInputWork instance); mov rax, [rcx]; call [rax+0x10]
    // (its per-frame Update) - the input manager's own call, from which both are taken.
    constexpr mgspwe::sites::Signature kSteamWorkSig { "stick: SteamInputWork update call", 0x2AD94, "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 10 48 8B 46 10", 0 };
    // float* Lookup(manager, group, name, player) -> the action's current value.
    constexpr mgspwe::sites::Signature kLookupSig { "stick: action lookup", 0x2B010, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 49 63 D9", 0 };
    // The look-input routine's own read of the manager global, for the manager pointer.
    constexpr mgspwe::sites::Signature kLookInputSig { "stick: look input, before the four lookups", 0x73E78A, "48 8B 0D ?? ?? ?? ?? 45 33 C9 BA DE 12 15 00 41 B8 79 4D BC 00 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ??", 0 };

    constexpr uint32_t kLookGroup = 0x1512DE;
    constexpr uint32_t kLookNames[4] = { 0xBC4D79, 0x2AD254, 0x480BDC, 0x26FC6E };   // X+, X-, Y+, Y-

    // An action's current value pointer is action + 0x68 + player * 0x24; its bindings are the
    // vector [action + 0x10, action + 0x18), and a binding is evaluated through vtable slot 8
    // (+0x40), exactly as the action update +2C720 does it.
    constexpr size_t kValueFromAction = 0x68, kBindingsBegin = 0x10, kBindingsEnd = 0x18;
    constexpr size_t kEvaluateSlot = 8;

    // The game's dead-zone band is [-47, +48) in units of 127, so the edge sits at 47.5; and its
    // gain of 1.6 makes full deflection come out at (127 - 48) * 1.6 = 126.4.
    constexpr float kShippedBandEdge = 47.5f, kShippedLookDeadZone = 48.0f, kShippedFullOut = 126.4f;

    using LookupFn = float* (*)(void* manager, uint32_t group, uint32_t name, int player);
    using EvaluateFn = float (*)(void* binding, int player);
    using UpdateFn = void (*)(void* self);

    SafetyHookMid g_LookDeadZone, g_LookGain, g_MoveBand;
    LookupFn g_Lookup = nullptr;
    void** g_Manager = nullptr;
    void** g_SteamWork = nullptr;
    uintptr_t g_ImageStart = 0, g_ImageEnd = 0;
    StickInput::Sample g_Sample;

    int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

    // A pointer the game handed us that lands inside the image is its static "no such action" zero
    // block, which must never be written.
    bool Writable(const void* p)
    {
        const uintptr_t a = reinterpret_cast<uintptr_t>(p);
        return p && !(a >= g_ImageStart && a < g_ImageEnd);
    }

    // The largest value over an action's bindings, the way the action update computes it.
    bool Recompute(float* value, float& out)
    {
        const uint8_t* action = reinterpret_cast<const uint8_t*>(value) - kValueFromAction;
        void* const* begin = nullptr; void* const* end = nullptr;
        std::memcpy(&begin, action + kBindingsBegin, sizeof(begin));
        std::memcpy(&end, action + kBindingsEnd, sizeof(end));
        if (!begin || !end || end < begin || (end - begin) > 64) { return false; }
        float best = 0;
        bool any = false;
        for (void* const* it = begin; it != end; ++it)
        {
            void* binding = *it;
            if (!binding) { continue; }
            void** vtable = *reinterpret_cast<void***>(binding);
            if (!vtable) { continue; }
            const auto evaluate = reinterpret_cast<EvaluateFn>(vtable[kEvaluateSlot]);
            if (!evaluate) { continue; }
            const float v = evaluate(binding, 0);
            if (!any || v > best) { best = v; any = true; }
        }
        out = best;
        return any;
    }
}

const StickInput::Sample& StickInput::Last() { return g_Sample; }

void StickInput::OnLookInput()
{
    if (!bLateSample || !g_Lookup || !g_Manager || !g_SteamWork) { return; }
    void* manager = *g_Manager;
    void* work = *g_SteamWork;
    if (!manager || !work) { return; }

    const int64_t start = Qpc();
    // Re-poll the pad: Input::SteamInputWork::Update, vtable slot 2 (+0x10), the same call the
    // input manager makes a frame's wait earlier.
    void** vtable = *reinterpret_cast<void***>(work);
    if (!vtable || !vtable[2]) { return; }
    reinterpret_cast<UpdateFn>(vtable[2])(work);

    // The actions were computed from the old poll, so recompute the four the look block is about
    // to read. This runs before the mouse's late sample, which then folds its accumulated
    // movement on top.
    int changed = 0;
    for (int i = 0; i < 4; i++)
    {
        float* value = g_Lookup(manager, kLookGroup, kLookNames[i], 0);
        if (!Writable(value)) { continue; }
        float fresh = 0;
        g_Sample.before[i] = *value;
        if (Recompute(value, fresh) && fresh != *value) { *value = fresh; changed++; }
        g_Sample.after[i] = *value;
    }
    g_Sample.serial++;
    g_Sample.polled = true;
    g_Sample.actionsChanged = changed;
    g_Sample.ticks = Qpc() - start;
}

void StickInput::Install()
{
    const bool deadZones = iLookDeadZone != static_cast<int>(kShippedLookDeadZone) || iMoveDeadZone != 47;
    if (!bLateSample && !deadZones) { return; }
    const uintptr_t base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
    MODULEINFO info {};
    GetModuleInformation(GetCurrentProcess(), mgs4e::game::Module(), &info, sizeof(info));
    g_ImageStart = base;
    g_ImageEnd = base + info.SizeOfImage;

    if (bLateSample)
    {
        const uintptr_t work = mgspwe::sites::Resolve(kSteamWorkSig);
        const uintptr_t lookup = mgspwe::sites::Resolve(kLookupSig);
        const uintptr_t look = mgspwe::sites::Resolve(kLookInputSig);
        if (work && lookup && look)
        {
            int32_t d = 0;
            std::memcpy(&d, reinterpret_cast<const uint8_t*>(base + work) + 3, 4);          // mov rcx, [rip+disp]
            g_SteamWork = const_cast<void**>(reinterpret_cast<void* const*>(base + work + 7 + d));
            std::memcpy(&d, reinterpret_cast<const uint8_t*>(base + look) + 3, 4);
            g_Manager = const_cast<void**>(reinterpret_cast<void* const*>(base + look + 7 + d));
            g_Lookup = reinterpret_cast<LookupFn>(base + lookup);
        }
        spdlog::info("PW stick: late sample ON: SteamInputWork global +{:X}, update slot 2, manager +{:X}, lookup +{:X} -> {}.",
            g_SteamWork ? reinterpret_cast<uintptr_t>(g_SteamWork) - base : 0,
            g_Manager ? reinterpret_cast<uintptr_t>(g_Manager) - base : 0, lookup,
            (g_SteamWork && g_Manager && g_Lookup) ? "ready" : "NOT INSTALLED");
    }

    if (iLookDeadZone != static_cast<int>(kShippedLookDeadZone))
    {
        const uintptr_t dz = mgspwe::sites::Resolve(kLookDeadZoneSig);
        const uintptr_t gain = mgspwe::sites::Resolve(kLookGainSig);
        if (dz && gain)
        {
            // xmm0 is the dead zone the four branches are about to subtract or add; xmm1 is the
            // gain both axes are about to be multiplied by. Rescaling the gain keeps full
            // deflection at the value the game produces today.
            g_LookDeadZone = safetyhook::create_mid(base + dz, [](SafetyHookContext& ctx)
            {
                ctx.xmm0.f32[0] = static_cast<float>(StickInput::iLookDeadZone);
            });
            g_LookGain = safetyhook::create_mid(base + gain, [](SafetyHookContext& ctx)
            {
                const float travel = 127.0f - static_cast<float>(StickInput::iLookDeadZone);
                ctx.xmm1.f32[0] = travel > 1.0f ? kShippedFullOut / travel : kShippedFullOut;
            });
        }
        spdlog::info("PW stick: look dead zone {} of 127 (the game's is 48): load +{:X} {}, gain +{:X} {}.",
            iLookDeadZone, dz, g_LookDeadZone ? "hooked" : "FAILED", gain, g_LookGain ? "hooked" : "FAILED");
    }

    if (iMoveDeadZone != 47)
    {
        const uintptr_t band = mgspwe::sites::Resolve(kMoveBandSig);
        if (band)
        {
            // The band itself is two byte compares against fixed immediates, so instead of moving
            // the band we scale what is measured against it: an axis counts as inside when
            // |axis| < iMoveDeadZone. Both registers are dead after the compares. Clamped to a
            // byte, because the compares only look at the low one and a wrap would land back
            // inside the band.
            g_MoveBand = safetyhook::create_mid(base + band, [](SafetyHookContext& ctx)
            {
                const float wanted = static_cast<float>(StickInput::iMoveDeadZone);
                const float k = wanted > 0.5f ? kShippedBandEdge / wanted : 1000.0f;
                const float a = (ctx.xmm1.f32[0] - 128.0f) * k + 128.0f;
                ctx.xmm1.f32[0] = std::clamp(a, 0.0f, 255.0f);
                const float b = (static_cast<float>(static_cast<int32_t>(ctx.rcx & 0xFFFFFFFF)) - 128.0f) * k + 128.0f;
                ctx.rcx = static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(std::clamp(b, 0.0f, 255.0f))));
            });
        }
        spdlog::info("PW stick: movement dead zone {} of 127 (the game's is 47): band +{:X} {}.",
            iMoveDeadZone, band, g_MoveBand ? "hooked" : "FAILED");
    }
}
