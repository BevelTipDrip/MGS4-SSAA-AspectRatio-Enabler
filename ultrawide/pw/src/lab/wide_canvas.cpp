#include "pch.hpp"
#include "wide_canvas.hpp"

#include "game.hpp"
#include "lab.hpp"
#include "log.hpp"
#include "mem.hpp"

#include <cmath>

namespace
{
    int g_UnitsW = 480, g_UnitsH = 272, g_Scale = 1;
    // Afevis's "UI scale": the canvas aspect over the native one. Above 1 the canvas is wider
    // (650x272 at 21:9: 1.354), below 1 it is taller (480x360 at 4:3: 0.756); his fixups
    // multiply by it on the wide side and divide by it on the tall side.
    float g_Ratio = 1.0f;
    float g_InvAspect = 272.0f / 480.0f;
    int g_OffsetX = 0, g_OffsetY = 0;
    bool g_Wider = true;

    struct Site
    {
        const char* name;
        uintptr_t rva;
        const char* bytes;   // expected bytes at the hook address (hex, spaces), a short prefix
    };

    // Every site is Afevis's pattern hit plus his offset (pw/tools/afevis_sites.py, clean dump 2026-09-12).
    constexpr Site kCameraConfig  { "Camera Config",       0x8EA17,  "4D 8B 13" };            // mov r10, [r11]
    constexpr Site kAspectRatio   { "Aspect Ratio",        0x8EE22,  "0F 28 DA F3 0F 5E 9B" }; // movaps xmm3, xmm2; divss xmm3, [rbx+0x250]
    constexpr Site kHudScaler     { "HUD Scaler",          0x563E3,  "F3 0F 59 96" };          // mulss xmm2, [rsi+..]
    constexpr Site kHudOffset     { "HUD Offset",          0x8EDE3,  "F3 0F 5E C2" };          // divss xmm0, xmm2
    constexpr Site kFov           { "FOV",                 0x8EBC8,  "F3 0F 11 B9" };          // movss [rcx+0x268], xmm7
    constexpr Site kCullingFov    { "Culling FOV",         0x8EE94,  "48 8D BB" };             // lea rdi, [rbx+0x1e0]
    constexpr Site kScissoring    { "Scissoring",          0x5A31F,  "89 9F" };                // mov [rdi+0x380], ebx
    constexpr Site kLoading       { "Loading Screens",     0x78609,  "E8" };                   // call (rdx, r8 = viewport centre)
    constexpr Site kMovies        { "Movies",              0x440FAB, "C7 44 24" };             // mov [rsp+..], imm
    constexpr Site kColorFilter   { "Color Filter",        0x2EF2C8, "66 89 85" };             // mov [rbp+..], ax
    constexpr Site kGlare         { "Glare",               0x19469B, "0F 28 CB" };             // movaps xmm1, xmm3
    constexpr Site kHalfGlare     { "Half Glare",          0x194A5A, "0F 28 CE" };             // movaps xmm1, xmm6
    constexpr Site kPickupMarkers { "Item Pickup Markers", 0xFEECA,  "" };                     // pattern + 105 (no fixed prefix)
    constexpr Site kDamageOverlay { "Damage Overlay",      0x491F0B, "0F 10 41" };             // movups xmm0, [rcx+..]
    constexpr Site kGrenadeArc    { "Grenade Arc",         0x6C3E28, "F3 0F 59 15" };          // mulss xmm2, [rip+..]
    constexpr Site kHitMarkers    { "Hit Markers",         0x6C1890, "F3 0F 11 46" };          // movss [rsi+..], xmm0

    SafetyHookMid g_Hooks[16] {};
    int g_HookCount = 0;

    bool BytesMatch(uintptr_t address, const char* hex)
    {
        if (!hex || !*hex) { return true; }
        const auto* p = reinterpret_cast<const uint8_t*>(address);
        int i = 0;
        for (const char* s = hex; *s; s += 3, i++)
        {
            const int v = std::stoi(std::string(s, 2), nullptr, 16);
            if (p[i] != v) { return false; }
            if (s[2] == '\0') { break; }
        }
        return true;
    }

    template <typename F>
    void Hook(const Site& site, F&& fn)
    {
        const auto address = reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + site.rva;
        if (!BytesMatch(address, site.bytes))
        {
            spdlog::warn("PW wide canvas: {} at +{:X} does not look as expected; not hooked.", site.name, site.rva);
            return;
        }
        auto& slot = g_Hooks[g_HookCount++];
        slot = safetyhook::create_mid(address, std::forward<F>(fn));
        spdlog::info("PW wide canvas: {} hooked at +{:X}{}.", site.name, site.rva, slot ? "" : " (FAILED)");
    }
}

namespace WideCanvas
{
    bool Active() { return g_UnitsW != 480 || g_UnitsH != 272; }
    int Units() { return g_UnitsW; }
    int HeightUnits() { return g_UnitsH; }

    void Install(int unitsW, int unitsH, int scale)
    {
        if (unitsW <= 0 || unitsH <= 0 || scale <= 0 || (unitsW == 480 && unitsH == 272)) { return; }
        g_UnitsW = unitsW; g_UnitsH = unitsH; g_Scale = scale;
        g_Ratio = (static_cast<float>(unitsW) / static_cast<float>(unitsH)) / (480.0f / 272.0f);
        g_Wider = g_Ratio > 1.0f;
        g_InvAspect = static_cast<float>(unitsH) / static_cast<float>(unitsW);
        g_OffsetX = (unitsW - 480) / 2;
        g_OffsetY = (unitsH - 272) / 2;
        spdlog::info("PW wide canvas: {}x{} units at scale {} ({}): UI offset ({}, {}), inverse aspect {:.5f}, ratio {:.5f}.",
            unitsW, unitsH, scale, g_Wider ? "wider than 16:9" : "taller than 16:9", g_OffsetX, g_OffsetY, g_InvAspect, g_Ratio);

        // The camera/UI structure (r10) once its magic is in place: the UI offset in units.
        Hook(kCameraConfig, [](SafetyHookContext& ctx)
        {
            if (!mgs4e::mem::Readable(reinterpret_cast<const void*>(ctx.r10), 584)) { return; }
            auto* p = reinterpret_cast<uint8_t*>(ctx.r10);
            if (*reinterpret_cast<int32_t*>(p + 576) != 67175190) { return; }
            *reinterpret_cast<int16_t*>(p + 580) = static_cast<int16_t>(g_OffsetX);
            *reinterpret_cast<int16_t*>(p + 582) = static_cast<int16_t>(g_OffsetY);
        });
        // The camera builder (rbx): inverse aspect and the canvas size.
        Hook(kAspectRatio, [](SafetyHookContext& ctx)
        {
            auto* p = reinterpret_cast<uint8_t*>(ctx.rbx);
            *reinterpret_cast<float*>(p + 592) = g_InvAspect;
            *reinterpret_cast<uint16_t*>(p + 584) = static_cast<uint16_t>(g_UnitsW);
            *reinterpret_cast<uint16_t*>(p + 586) = static_cast<uint16_t>(g_UnitsH);
        });
        // The UI scale: our integer scale, whatever the width immediate divides to.
        Hook(kHudScaler, [](SafetyHookContext& ctx)
        {
            ctx.xmm6.f32[0] = static_cast<float>(g_Scale);
            ctx.xmm2.f32[0] = static_cast<float>(g_Scale);
        });
        // The game's own centring term, zeroed so the offset above is not applied twice.
        Hook(kHudOffset, [](SafetyHookContext& ctx)
        {
            ctx.xmm0.u32[0] = 0;
            ctx.rax = 0;
        });
        // FOV (wider only): after the raw value is stored, the copy used for the projection is
        // divided by the ratio, so the world fills the wider canvas at the same vertical extent.
        // A taller canvas keeps the FOV: the world shows more above and below.
        Hook(kFov, [](SafetyHookContext& ctx) { if (g_Wider) { ctx.xmm6.f32[0] /= g_Ratio; } });
        Hook(kCullingFov, [](SafetyHookContext& ctx) { if (g_Wider) { ctx.xmm0.f32[0] /= g_Ratio; } });
        // The output-shaped scissor: the width (wider) or the height (taller) term.
        Hook(kScissoring, [](SafetyHookContext& ctx)
        {
            if (g_Wider) { ctx.rax = static_cast<uint64_t>(std::llround(static_cast<double>(static_cast<int64_t>(ctx.rax)) * 2.0 * g_Ratio)); }
            else { ctx.rbx = static_cast<uint64_t>(std::llround(static_cast<double>(static_cast<int64_t>(ctx.rbx)) * 2.0 / g_Ratio)); }
        });
        // Loading screens: PSP viewport centre 2048 plus the offset. Movies: the offset in registers.
        Hook(kLoading, [](SafetyHookContext& ctx)
        {
            ctx.rdx = static_cast<uint64_t>(g_OffsetX + 2048);
            ctx.r8 = static_cast<uint64_t>(g_OffsetY + 2048);
        });
        Hook(kMovies, [](SafetyHookContext& ctx)
        {
            ctx.rcx = static_cast<uint64_t>(g_OffsetX);
            ctx.rdx = static_cast<uint64_t>(g_OffsetY);
        });
        // Colour filter: canvas size into its structure (rbp) and the height in rax.
        Hook(kColorFilter, [](SafetyHookContext& ctx)
        {
            auto* p = reinterpret_cast<uint8_t*>(ctx.rbp);
            *reinterpret_cast<uint16_t*>(p + 250) = static_cast<uint16_t>(g_UnitsW);
            *reinterpret_cast<uint16_t*>(p + 254) = static_cast<uint16_t>(g_UnitsW);
            *reinterpret_cast<uint16_t*>(p + 256) = static_cast<uint16_t>(g_UnitsH);
            ctx.rax = static_cast<uint64_t>(static_cast<uint32_t>(g_UnitsH));
        });
        // Glare and the mirrored half glare: the edge term in the wider or taller canvas.
        Hook(kGlare, [](SafetyHookContext& ctx)
        {
            if (g_Wider) { ctx.xmm13.f32[0] *= g_Ratio; }
            else if (mgs4e::mem::Readable(reinterpret_cast<const void*>(ctx.rsp + 40), sizeof(float))) { *reinterpret_cast<float*>(ctx.rsp + 40) /= g_Ratio; }
        });
        Hook(kHalfGlare, [](SafetyHookContext& ctx)
        {
            if (g_Wider) { ctx.xmm10.f32[0] = g_Ratio * 240.0f - ctx.xmm13.f32[0]; }
            else { ctx.xmm11.f32[0] /= g_Ratio; }
        });
        // Item pickup markers: the world-to-screen result shifted back by half the widening.
        Hook(kPickupMarkers, [](SafetyHookContext& ctx)
        {
            if (mgs4e::mem::Readable(reinterpret_cast<const void*>(ctx.r8), sizeof(float)))
            {
                *reinterpret_cast<float*>(ctx.r8) -= static_cast<float>(g_OffsetX);
            }
            ctx.xmm3.f32[0] -= static_cast<float>(g_OffsetY);
        });
        // Damage overlay: eight fields of canvas size in its structure (rcx).
        Hook(kDamageOverlay, [](SafetyHookContext& ctx)
        {
            if (!mgs4e::mem::Readable(reinterpret_cast<const void*>(ctx.rcx), 472)) { return; }
            auto* p = reinterpret_cast<uint8_t*>(ctx.rcx);
            *reinterpret_cast<uint16_t*>(p + 210) = static_cast<uint16_t>(g_UnitsW);
            *reinterpret_cast<uint16_t*>(p + 212) = static_cast<uint16_t>(g_UnitsH);
            *reinterpret_cast<uint32_t*>(p + 324) = static_cast<uint32_t>(g_UnitsW);
            *reinterpret_cast<uint32_t*>(p + 452) = static_cast<uint32_t>(g_UnitsW);
            *reinterpret_cast<uint16_t*>(p + 340) = static_cast<uint16_t>(g_UnitsW);
            *reinterpret_cast<uint16_t*>(p + 342) = static_cast<uint16_t>(g_UnitsH);
            *reinterpret_cast<uint16_t*>(p + 468) = static_cast<uint16_t>(g_UnitsW);
            *reinterpret_cast<uint16_t*>(p + 470) = static_cast<uint16_t>(g_UnitsH);
        });
        // Grenade arc and hit markers: one register scaled by the ratio (the other divided when taller).
        Hook(kGrenadeArc, [](SafetyHookContext& ctx)
        {
            if (g_Wider) { ctx.xmm2.f32[0] *= g_Ratio; } else { ctx.xmm0.f32[0] /= g_Ratio; }
        });
        Hook(kHitMarkers, [](SafetyHookContext& ctx)
        {
            if (g_Wider) { ctx.xmm0.f32[0] *= g_Ratio; } else { ctx.xmm2.f32[0] /= g_Ratio; }
        });
        spdlog::info("PW wide canvas: {} hooks installed.", g_HookCount);
    }
}
