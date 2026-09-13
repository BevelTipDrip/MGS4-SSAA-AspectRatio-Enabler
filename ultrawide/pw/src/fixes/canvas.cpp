#include "pch.hpp"
#include "canvas.hpp"

#include "game.hpp"
#include "lab.hpp"
#include "log.hpp"
#include "mem.hpp"

#include <atomic>
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
    constexpr Site kCameraConfig  { "camera config",       0x8EA17,  "4D 8B 13" };            // mov r10, [r11]
    constexpr Site kAspectRatio   { "aspect ratio",        0x8EE22,  "0F 28 DA F3 0F 5E 9B" }; // movaps xmm3, xmm2; divss xmm3, [rbx+0x250]
    constexpr Site kHudScaler     { "HUD scaler",          0x563E3,  "F3 0F 59 96" };          // mulss xmm2, [rsi+..]
    constexpr Site kHudOffset     { "HUD offset",          0x8EDE3,  "F3 0F 5E C2" };          // divss xmm0, xmm2
    constexpr Site kFov           { "FOV",                 0x8EBC8,  "F3 0F 11 B9" };          // movss [rcx+0x268], xmm7
    constexpr Site kCullingFov    { "culling FOV",         0x8EE94,  "48 8D BB" };             // lea rdi, [rbx+0x1e0]
    constexpr Site kScissoring    { "scissoring",          0x5A31F,  "89 9F" };                // mov [rdi+0x380], ebx
    constexpr Site kLoading       { "loading screens",     0x78609,  "E8" };                   // call (rdx, r8 = viewport centre)
    constexpr Site kMovies        { "movies",              0x440FAB, "C7 44 24" };             // mov [rsp+..], imm
    constexpr Site kColorFilter   { "colour filter",       0x2EF2C8, "66 89 85" };             // mov [rbp+..], ax
    constexpr Site kGlare         { "glare",               0x19469B, "0F 28 CB" };             // movaps xmm1, xmm3
    constexpr Site kHalfGlare     { "half glare",          0x194A5A, "0F 28 CE" };             // movaps xmm1, xmm6
    constexpr Site kPickupMarkers { "item pickup markers", 0xFEECA,  "" };                     // pattern + 105 (no fixed prefix)
    constexpr Site kDamageOverlay { "damage overlay",      0x491F0B, "0F 10 41" };             // movups xmm0, [rcx+..]
    constexpr Site kGrenadeArc    { "grenade arc",         0x6C3E28, "F3 0F 59 15" };          // mulss xmm2, [rip+..]
    constexpr Site kHitMarkers    { "hit markers",         0x6C1890, "F3 0F 11 46" };          // movss [rsi+..], xmm0

    SafetyHookMid g_Hooks[16] {};
    int g_HookCount = 0, g_HookFailures = 0;

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
            g_HookFailures++;
            spdlog::warn("PW canvas: the {} site does not look as expected; that fixup is off.", site.name);
            return;
        }
        auto& slot = g_Hooks[g_HookCount++];
        slot = safetyhook::create_mid(address, std::forward<F>(fn));
        if (!slot) { g_HookFailures++; spdlog::warn("PW canvas: the {} hook could not be installed.", site.name); }
#if MGS4E_LAB_BUILD
        else { spdlog::info("PW canvas: {} hooked at +{:X}.", site.name, site.rva); }
#endif
    }

    // The in-mission HUD register (docs/ui-elements.md 5.3): each group's translation row and
    // the side it belongs to on a wider canvas (-1 left, +1 right) and on a taller one (-1 top,
    // +1 bottom). The move is half the canvas growth toward that side, so the group keeps the
    // 16:9 distance from the display edge.
    struct HudGroup { float tx, ty; int sideWide, sideTall; };
    constexpr HudGroup kHudRegister[] = {
        { -11.0f,  -8.0f, -1, -1 },   // mission timer, remaining enemies
        {   8.0f, -12.0f, -1, +1 },   // life bar and name
        {  -6.0f,  38.0f, +1, +1 },   // item and weapon column
        {  -6.0f,  32.0f, +1, +1 },
        {  -6.0f,   6.0f, +1, +1 },
        {  -6.0f,   7.0f, +1, +1 },
        {  -6.0f,  12.0f, +1, +1 },
        {  -3.0f,   6.0f, +1, +1 },
        {  -5.0f,  32.0f, +1, +1 },   // item count
        { 112.0f,   4.0f, +1, -1 },   // top-right gauge
        { 454.8f,  65.5f, +1, -1 },   // gauge percentage
        { 133.0f,  58.0f, +1, +1 },   // weapon box
        { 133.0f,  85.0f, +1, +1 },   // item box
    };
    float g_MoveX = 0, g_MoveY = 0;   // half the canvas growth, in units
    std::atomic<uint32_t> g_PostQuads { 0 };
}

namespace Canvas
{
    bool Active() { return g_UnitsW != 480 || g_UnitsH != 272; }
    int Units() { return g_UnitsW; }
    int HeightUnits() { return g_UnitsH; }

    void Configure(int unitsW, int unitsH, int scale)
    {
        if (unitsW <= 0 || unitsH <= 0 || scale <= 0 || (unitsW == 480 && unitsH == 272)) { return; }
        g_UnitsW = unitsW; g_UnitsH = unitsH; g_Scale = scale;
        g_Ratio = (static_cast<float>(unitsW) / static_cast<float>(unitsH)) / (480.0f / 272.0f);
        g_Wider = g_Ratio > 1.0f;
        g_InvAspect = static_cast<float>(unitsH) / static_cast<float>(unitsW);
        g_OffsetX = (unitsW - 480) / 2;
        g_OffsetY = (unitsH - 272) / 2;
        g_MoveX = static_cast<float>(g_OffsetX);
        g_MoveY = static_cast<float>(g_OffsetY);

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
        // The UI scale: the integer scale, whatever the width immediate divides to.
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
        spdlog::info("PW canvas: {}x{} units at scale {} ({} than 16:9): UI offset ({}, {}), {} fixups{}; HUD groups move {} sideways, {} vertically.",
            unitsW, unitsH, scale, g_Wider ? "wider" : "taller", g_OffsetX, g_OffsetY, g_HookCount,
            g_HookFailures ? std::format(" ({} off)", g_HookFailures) : std::string(), g_MoveX, g_MoveY);
    }

    bool OnConstantUnmap(void* data, size_t size)
    {
        if (size < 32 * sizeof(float)) { return false; }
        float* f = static_cast<float*>(data);
        // Only the UI ortho (row 1 = 2/480): leave 3D alone. The world basis (rows 5-7) may be
        // rotated (vertical text) or scaled (gauges); the translation row is canvas units
        // regardless, so it is always the handle.
        if (std::fabs(f[0] - 2.0f / 480.0f) > 1e-5f) { return false; }
        if (!Active()) { return true; }
        for (const HudGroup& g : kHudRegister)
        {
            if (std::fabs(f[28] - g.tx) > 0.5f || std::fabs(f[29] - g.ty) > 0.5f) { continue; }
            f[28] += g_MoveX * static_cast<float>(g.sideWide);
            f[29] += g_MoveY * static_cast<float>(g.sideTall);
        }
        return true;
    }

    void OnVertexUnmap(void* data, size_t size)
    {
        // The post chain's full-canvas quads: six 20-byte vertices (u, v, x, y, z), 120 bytes
        // plus up to 8 of padding. The downscale into the 640x640 blur target samples the scene
        // over u 0..480 / v 0..272 (canvas units of the source), the composite draws over x
        // 0..480 / y 0..272; all hard-sized, so on a wider canvas the blur covered 74% of a 21:9
        // frame. Each 480 becomes the canvas width, each 272 the canvas height.
        if (!Active() || size < 120 || size > 256 || (size % 20 != 0 && size % 20 != 8)) { return; }
        auto* f = static_cast<float*>(data);
        const float unitsW = static_cast<float>(g_UnitsW), unitsH = static_cast<float>(g_UnitsH);
        bool any = false;
        for (size_t i = 0; i < size / 20; i++)
        {
            if (std::fabs(f[i * 5 + 2] - 480.0f) < 0.01f) { f[i * 5 + 2] = unitsW; any = true; }
            if (std::fabs(f[i * 5 + 0] - 480.0f) < 0.01f) { f[i * 5 + 0] = unitsW; any = true; }
            if (std::fabs(f[i * 5 + 3] - 272.0f) < 0.01f) { f[i * 5 + 3] = unitsH; any = true; }
            if (std::fabs(f[i * 5 + 1] - 272.0f) < 0.01f) { f[i * 5 + 1] = unitsH; any = true; }
        }
        if (any && g_PostQuads.fetch_add(1) == 0 && mgs4e::log::Verbose()) { spdlog::info("PW canvas: post-chain quads resized to {}x{} units.", unitsW, unitsH); }
    }
}
