#include "pch.hpp"
#include "internal_size.hpp"
#include <atomic>
#include <cmath>

#include "game.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "sites.hpp"
#include "canvas.hpp"


namespace
{
    // Every site that turns the -resolution index into the internal size carries both sizes as
    // immediates (the "1" branch first, then the "0" branch). Offsets are of the immediate
    // itself, measured on the live dump of 2026-09-13 (build 1.3.1.0).
    struct Imm32Site { uintptr_t rva; uint32_t expected; bool isWidth; bool hudScale; mgspwe::sites::Signature sig; };   // rva resolved from sig at Apply
    // Getter 1 (+16201) is read by the function that creates the post-chain targets (the
    // resolved colour, the HDR target, the post depth: creation stacks +1642D/+1677C/+16801/
    // +16A38 under +23200); it takes the post size. The other sites size and place the scene
    // (HUD scale, resolve, viewport getters) and take the scene size.
    Imm32Site kPostImm32[] = {   // target getter 1
        { 0, 0x780, true, false, { "post size: getter 1 w1", 0x1620E, "74 0E 41 BC 80 07 00 00 41 BD 40 04 00 00", 4 } }, { 0, 0x440, false, false, { "post size: getter 1 h1", 0x16214, "41 BC 80 07 00 00 41 BD 40 04 00 00 EB 0C 41 BC A0 05 00 00", 8 } },
        { 0, 0x5A0, true, false, { "post size: getter 1 w0", 0x1621C, "EB 0C 41 BC A0 05 00 00 41 BD 30 03 00 00", 4 } }, { 0, 0x330, false, false, { "post size: getter 1 h0", 0x16222, "41 BC A0 05 00 00 41 BD 30 03 00 00 8B 05 ?? ?? ?? ??", 8 } },
    };
    Imm32Site kImm32[] = {
        { 0, 0x780, true, false, { "scene size: getter 3 w1", 0x1AC4B, "74 0E 41 B9 80 07 00 00 41 BA 40 04 00 00", 4 } }, { 0, 0x440, false, false, { "scene size: getter 3 h1", 0x1AC51, "41 B9 80 07 00 00 41 BA 40 04 00 00 EB 0C 41 B9 A0 05 00 00", 8 } },   // target getter 3
        { 0, 0x5A0, true, false, { "scene size: getter 3 w0", 0x1AC59, "EB 0C 41 B9 A0 05 00 00 41 BA 30 03 00 00", 4 } }, { 0, 0x330, false, false, { "scene size: getter 3 h0", 0x1AC5F, "41 B9 A0 05 00 00 41 BA 30 03 00 00 F3 0F 10 2D ?? ?? ?? ??", 8 } },
        { 0, 0x780, true, false, { "scene size: getter 2 w1", 0x5201A, "74 0C B8 80 07 00 00 BA 40 04 00 00", 3 } }, { 0, 0x440, false, false, { "scene size: getter 2 h1", 0x5201F, "B8 80 07 00 00 BA 40 04 00 00 EB 0A B8 A0 05 00 00", 6 } },   // target getter 2
        { 0, 0x5A0, true, false, { "scene size: getter 2 w0", 0x52026, "EB 0A B8 A0 05 00 00 BA 30 03 00 00", 3 } }, { 0, 0x330, false, false, { "scene size: getter 2 h0", 0x5202B, "B8 A0 05 00 00 BA 30 03 00 00 F3 0F 10 15 ?? ?? ?? ??", 6 } },
        { 0, 0x780, true, true, { "scene size: HUD scale w1", 0x56456, "8B 91 6C 29 00 00 B9 80 07 00 00 4C 89 B4 24 C8 00 00 00", 7 } }, { 0, 0x5A0, true, true, { "scene size: HUD scale w0", 0x56477, "75 05 B9 A0 05 00 00 48 8B 9F 40 29 00 00", 3 } },   // HUD scale (width only)
    };
    struct Imm64Site { uintptr_t rva; uint64_t expected; mgspwe::sites::Signature sig; };
    Imm64Site kImm64[] = {
        { 0, 0x44000000780ull, { "scene size: packed getter 1", 0x3F659, "48 8B 0D ?? ?? ?? ?? 48 B8 80 07 00 00 40 04 00 00", 9 } }, { 0, 0x330000005A0ull, { "scene size: packed getter 0", 0x3F66D, "75 0A 48 B8 A0 05 00 00 30 03 00 00", 4 } },   // packed getter
        { 0, 0x44000000780ull, { "scene size: target creation 1", 0x5C37C, "41 0F 29 7B A8 48 BB 80 07 00 00 40 04 00 00", 7 } }, { 0, 0x330000005A0ull, { "scene size: target creation 0", 0x5C395, "75 0A 48 BB A0 05 00 00 30 03 00 00", 4 } },   // internal target creation
    };
    // The render-scale getter (+3F530): returns (scaleY << 32 | scaleX) as an immediate per
    // -resolution value; every scene target is the 480x272 canvas times it (4x MSAA on the
    // scene colour target), so it must stay an integer.
    Imm64Site kScale[] = { { 0, 0x400000004ull, { "render scale: immediate 1", 0x3F689, "48 8B 0D ?? ?? ?? ?? 48 B8 04 00 00 00 04 00 00 00", 9 } }, { 0, 0x300000003ull, { "render scale: immediate 0", 0x3F69D, "75 0A 48 B8 03 00 00 00 03 00 00 00", 4 } } };
    constexpr uintptr_t kOutputTable = 0xD8F1C8;   // four (width, height) int pairs, .rdata
    constexpr int kOutputSlots = 4;
    // Just after the game's picture fit (+1A2B5..+1A360) has written the picture size and
    // offsets into the display object (rsi): picture w/h at +0x2940/+0x2944, x/y offset at
    // +0x2948/+0x294c. With the back buffer at the internal size the picture is the whole buffer.
    constexpr mgspwe::sites::Signature kAfterFitSig { "window: after fit", 0x1A366, "B9 11 00 00 00 8B 96 78 29 00 00 89 96 7C 29 00 00", 0 };   // mov ecx, 0x11
    uintptr_t kAfterFit = 0;
    SafetyHookMid g_AfterFit {};
    // Just after the Fullscreen (mode 2) display-mode loop (+1A220..+1A259): the game has
    // walked EnumDisplaySettingsA and kept the largest width (r15d) and height (r14d) of any
    // mode the monitor advertises, and is about to SetWindowPos the window to that size and
    // create the exclusive chain from it. A 3840x2160 panel that advertises a 4096x2160 mode
    // gets the game at 4096x2160, whatever the desktop runs at (a user's report, 2026-09-13).
    // The selected screen resolution replaces the pair, or the desktop's current mode when
    // none is set.
    constexpr mgspwe::sites::Signature kAfterModeLoopSig { "window: after mode loop", 0x1A25B, "44 8B 4D A8 33 D2 44 8B 45 A4 48 8B 0D ?? ?? ?? ??", 0 };   // mov r9d, [rbp-0x58]
    uintptr_t kAfterModeLoop = 0;
    SafetyHookMid g_AfterModeLoop {};
    // The two sites that turn the integer scale into a float: the game scaler (+596E5,
    // `cvtsi2ss xmm1, rcx` after the getter; replaced by our value, the instruction skipped) and
    // the render-scale store (+5B015, `addss xmm0, 0.5` then truncation; xmm0 replaced first).
    // Forcing a fraction here reproduces the fractional scale of Afevis's patch on purpose.
    constexpr mgspwe::sites::Signature kGameScalerSig { "internal size: game scaler", 0x59835, "F3 48 0F 2A C9 EB 03 0F 28 CE 41 8B 46 28", 0 };
    constexpr mgspwe::sites::Signature kRenderScaleStoreSig { "internal size: render scale store", 0x5B165, "F3 0F 58 05 ?? ?? ?? ?? F3 0F 2C C0 89 41 50 C6 81 F0 00 00 00 01", 0 };
    uintptr_t kGameScaler = 0, kRenderScaleStore = 0;
    SafetyHookMid g_GameScaler {}, g_RenderScaleStore {};
    // Scene target creation (+89F8D): edi = scale x canvas width, ebx = scale x canvas height,
    // the descriptor in r15 (+0x1c width, +0x18 height in canvas units). Afevis replaces the
    // full-canvas targets' size here with the output size; the experiment does the same.
    constexpr mgspwe::sites::Signature kSceneCreateSig { "internal size: scene create", 0x8A10D, "E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 84 C0 74 34 83 BC 24 A8 00 00 00 01", 0 };
    uintptr_t kSceneCreate = 0;
    SafetyHookMid g_SceneCreate {};
    // The settings getter (+84D50, a 14-byte leaf: `mov rax,[array]; movsxd rdx,ecx; mov eax,[rax+rdx*4]`)
    // and setter (+85130). The window code reads id 3 (display mode), 4/5 (position), 7/8
    // (size), 17 (monitor), 19 (windowed preset) and 246 (applied mode) through the getter;
    // the in-game Options menu persists the mode through the setter. The game's numbering:
    // 0 Borderless (frameless, the monitor), 1 Windowed, 2 Fullscreen (exclusive; user-verified
    // against the in-game menu 2026-09-13). Mode 2 creates an
    // exclusive-fullscreen chain at the display mode Windows offers: on a monitor whose native
    // resolution is 16:9 that is the native mode rather than a 21:9 desktop resolution (a
    // native 21:9 output is fine), so the Lab can pin the mode and logs the saved values once.
    constexpr mgspwe::sites::Signature kSettingsGetSig { "window: settings get", 0x84ED0, "48 8B 05 ?? ?? ?? ?? 48 63 D1 8B 04 90 C3 CC CC", 0 };
    constexpr mgspwe::sites::Signature kSettingsSetSig { "window: settings set", 0x852B0, "8B 05 ?? ?? ?? ?? 41 BA 01 00 00 00 4C 8B 05 ?? ?? ?? ??", 0 };
    uintptr_t kSettingsGet = 0, kSettingsSet = 0;
    constexpr uintptr_t kSettingsArray = 0x10C58A0;
    SafetyHookInline g_SettingsGet {}, g_SettingsSet {};
    std::atomic<bool> g_SettingsLogged { false };
    std::atomic<bool> g_ModeWritten { false };

    int __fastcall Hooked_SettingsGet(int id)
    {
        if (!g_SettingsLogged.exchange(true))
        {
            const auto* array = *reinterpret_cast<const int32_t* const*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kSettingsArray);
            if (array)
            {
                spdlog::info("PW window: saved settings: mode(3)={} pos(4,5)={},{} size(7,8)={}x{} monitor(17)={} preset(19)={} applied mode(246)={}.",
                    array[3], array[4], array[5], array[7], array[8], array[17], array[19], array[246]);
            }
        }
        // The selected window mode is written into the game's settings array once, on the
        // first read of the mode after the saved settings are loaded, and nothing is overridden
        // after that: the in-game Options menu shows the running mode and can still change it
        // (its runtime switch handler does the work), and the game persists whatever it ends on.
        if (id == 3 && InternalSize::iWindowMode >= 0 && !g_ModeWritten.exchange(true))
        {
            auto* array = *reinterpret_cast<int32_t**>(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kSettingsArray);
            if (array)
            {
                spdlog::info("PW window: saved display mode {} replaced by {} for this run.", array[3], InternalSize::iWindowMode);
                array[3] = InternalSize::iWindowMode;
            }
        }
        return g_SettingsGet.fastcall<int>(id);
    }

    void __fastcall Hooked_SettingsSet(int id, int value)
    {
        if (id == 3 || id == 246) { spdlog::info("PW window: game writes setting {} = {}.", id, value); }
        g_SettingsSet.fastcall<void>(id, value);
    }
    int g_SceneW = 0, g_SceneH = 0;         // the full-canvas targets' size when overridden (wide canvas, or the Lab experiment)
    int g_PostW = 0, g_PostH = 0;           // the post chain's target size as applied
    int g_CanvasW = 480, g_CanvasH = 272;   // the canvas in PSP units for this run (the fit hook reads it)
    float g_FloatScale = 0;

    template <typename T>
    bool PatchChecked(uintptr_t rva, T expected, T value, const char* what)
    {
        if (!rva) { return false; }   // unresolved in this build (already logged)
        const auto address = reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + rva;
        T current {};
        std::memcpy(&current, reinterpret_cast<const void*>(address), sizeof(T));
        if (current != expected)
        {
            spdlog::warn("PW internal size: {} at +{:X} holds {:#x}, expected {:#x}; left alone.", what, rva, static_cast<uint64_t>(current), static_cast<uint64_t>(expected));
            return false;
        }
        mgs4e::mem::Poke<T>(address, value);
        return true;
    }
}

namespace InternalSize
{
    int BackBufferWidth() { return bBackBufferAtInternal ? g_PostW : 0; }
    int BackBufferHeight() { return bBackBufferAtInternal ? g_PostH : 0; }

    void Apply()
    {
        // Every site by signature first (sites.hpp); an unresolved one leaves its feature off.
        for (Imm32Site& s : kPostImm32) { s.rva = mgspwe::sites::Resolve(s.sig); }
        for (Imm32Site& s : kImm32) { s.rva = mgspwe::sites::Resolve(s.sig); }
        for (Imm64Site& s : kImm64) { s.rva = mgspwe::sites::Resolve(s.sig); }
        for (Imm64Site& s : kScale) { s.rva = mgspwe::sites::Resolve(s.sig); }
        kAfterFit = mgspwe::sites::Resolve(kAfterFitSig);
        kAfterModeLoop = mgspwe::sites::Resolve(kAfterModeLoopSig);
        kGameScaler = mgspwe::sites::Resolve(kGameScalerSig);
        kRenderScaleStore = mgspwe::sites::Resolve(kRenderScaleStoreSig);
        kSceneCreate = mgspwe::sites::Resolve(kSceneCreateSig);
        kSettingsGet = mgspwe::sites::Resolve(kSettingsGetSig);
        kSettingsSet = mgspwe::sites::Resolve(kSettingsSetSig);
        // The wide canvas is one switch: off means a plain 16:9 run whatever the units say; on
        // with no units takes the primary display's aspect: wider than 16:9 widens the canvas
        // (650x272 at 21:9), narrower makes it taller (480x360 at 4:3, 480x300 at 16:10). An
        // output size of 0 becomes the display size, so one toggle moves between the runs.
        int canvasW = 480, canvasH = 272;
        if (bWideCanvas)
        {
            const int screenW = GetSystemMetrics(SM_CXSCREEN), screenH = GetSystemMetrics(SM_CYSCREEN);
            // The canvas follows the picture: an explicit output size sets the aspect, else the display.
            const bool outputGiven = iOutputWidth > 0 && iOutputHeight > 0;
            const double aspect = outputGiven ? static_cast<double>(iOutputWidth) / iOutputHeight
                                : (screenW > 0 && screenH > 0) ? static_cast<double>(screenW) / screenH : 480.0 / 272.0;
            // A 16:9 display (1.7778) is a shade wider than the 480x272 canvas (1.7647): that is
            // the game's own shape and stays 480x272, or a 484-unit canvas would shift the UI.
            const bool sixteenNine = aspect >= 480.0 / 272.0 - 0.005 && aspect <= 16.0 / 9.0 + 0.005;
            const bool wider = aspect > 16.0 / 9.0 + 0.005;
            canvasW = iWideCanvasUnits > 0 ? iWideCanvasUnits : (sixteenNine ? 480 : wider ? static_cast<int>(std::lround(272.0 * aspect)) : 480);
            canvasH = iWideCanvasHeightUnits > 0 ? iWideCanvasHeightUnits : (sixteenNine || wider ? 272 : static_cast<int>(std::lround(480.0 / aspect)));
            if (canvasW <= 480 && canvasH <= 272)
            {
                spdlog::info("PW wide canvas: on, but the canvas would be {}x{} units (16:9): off for this run.", canvasW, canvasH);
                canvasW = 480; canvasH = 272;
            }
            else
            {
                if (iOutputWidth <= 0 || iOutputHeight <= 0) { iOutputWidth = screenW; iOutputHeight = screenH; }
                spdlog::info("PW wide canvas: on: {}x{} units (display {}x{}), output {}x{}.", canvasW, canvasH, screenW, screenH, iOutputWidth, iOutputHeight);
            }
        }
        const bool canvasActive = (canvasW != 480 || canvasH != 272);
        iWideCanvasUnits = canvasActive ? canvasW : 0;
        iWideCanvasHeightUnits = canvasActive ? canvasH : 0;
        g_CanvasW = canvasW; g_CanvasH = canvasH;
        if (iRenderScale > 0)
        {
            const uint64_t packed = (static_cast<uint64_t>(iRenderScale) << 32) | static_cast<uint32_t>(iRenderScale);
            int ok = 0;
            for (const Imm64Site& s : kScale) { ok += PatchChecked<uint64_t>(s.rva, s.expected, packed, "render scale immediate"); }
            spdlog::info("PW internal size: render scale set to {} at {} of {} sites.", iRenderScale, ok, std::size(kScale));
        }
        // Scene size: the render scale's canvas multiple when set, else the explicit internal size.
        // With a wide canvas (private module) the scene targets are units x scale wide, but the
        // HUD scale sites keep 480 x scale: the UI is still laid out in 480 units at the same scale.
        const int sceneW = iRenderScale > 0 ? canvasW * iRenderScale : iInternalWidth;
        const int sceneH = iRenderScale > 0 ? canvasH * iRenderScale : iInternalHeight;
        // The two width sites of the UI-scale function follow the scene: with a wide canvas the
        // private module pins the UI scale itself (the engine's own canvas fields do the rest).
        const int hudW = sceneW;
        int sceneReqW = iSceneWidth, sceneReqH = iSceneHeight;   // the Lab experiment's explicit size, if any
        if (canvasActive && iRenderScale > 0 && sceneReqW == 0 && sceneReqH == 0)
        {
            sceneReqW = sceneW; sceneReqH = sceneH;   // the full-canvas targets must follow the canvas
        }
        // Post size: the explicit internal size when given together with a render scale, else the scene size.
        const int postW = (iRenderScale > 0 && iInternalWidth > 0) ? iInternalWidth : sceneW;
        const int postH = (iRenderScale > 0 && iInternalHeight > 0) ? iInternalHeight : sceneH;
        if (sceneW > 0 && sceneH > 0)
        {
            int ok = 0;
            for (const Imm32Site& s : kImm32)
            {
                ok += PatchChecked<uint32_t>(s.rva, s.expected, static_cast<uint32_t>(s.isWidth ? (s.hudScale ? hudW : sceneW) : sceneH), "scene size immediate");
            }
            const uint64_t packed = (static_cast<uint64_t>(static_cast<uint32_t>(sceneH)) << 32) | static_cast<uint32_t>(sceneW);
            for (const Imm64Site& s : kImm64)
            {
                ok += PatchChecked<uint64_t>(s.rva, s.expected, packed, "packed scene size immediate");
            }
            for (const Imm32Site& s : kPostImm32)
            {
                ok += PatchChecked<uint32_t>(s.rva, s.expected, static_cast<uint32_t>(s.isWidth ? postW : postH), "post size immediate");
            }
            spdlog::info("PW internal size: scene {}x{} (HUD scale width {}), post chain {}x{}, at {} of {} sites.", sceneW, sceneH, hudW, postW, postH, ok, std::size(kImm32) + std::size(kImm64) + std::size(kPostImm32));
            g_PostW = postW; g_PostH = postH;
        }
        if (iRenderScaleHundredths > 0)
        {
            g_FloatScale = static_cast<float>(iRenderScaleHundredths) / 100.0f;
            const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
            const uint8_t* a = reinterpret_cast<const uint8_t*>(base + kGameScaler);
            const uint8_t* b = reinterpret_cast<const uint8_t*>(base + kRenderScaleStore);
            if (kGameScaler && kRenderScaleStore && a[0] == 0xF3 && a[1] == 0x48 && a[2] == 0x0F && a[3] == 0x2A && b[0] == 0xF3 && b[1] == 0x0F && b[2] == 0x58)
            {
                g_GameScaler = safetyhook::create_mid(base + kGameScaler, [](SafetyHookContext& ctx) { ctx.xmm1.f32[0] = g_FloatScale; ctx.rip += 5; });
                g_RenderScaleStore = safetyhook::create_mid(base + kRenderScaleStore, [](SafetyHookContext& ctx) { ctx.xmm0.f32[0] = g_FloatScale; });
                spdlog::info("PW internal size: EXPERIMENT: float render scale forced to {:.2f} at the game scaler and the render-scale store ({}/{}).", g_FloatScale, g_GameScaler ? "ok" : "FAILED", g_RenderScaleStore ? "ok" : "FAILED");
            }
            else { spdlog::warn("PW internal size: float scale sites do not look as expected; experiment not installed."); }
        }
        if (sceneReqW > 0 && sceneReqH > 0)
        {
            g_SceneW = sceneReqW; g_SceneH = sceneReqH;
            const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
            const uint8_t* c = reinterpret_cast<const uint8_t*>(base + kSceneCreate);
            if (kSceneCreate && c[0] == 0xE8)   // call +17D20
            {
                g_SceneCreate = safetyhook::create_mid(base + kSceneCreate, [](SafetyHookContext& ctx)
                {
                    const auto* desc = reinterpret_cast<const int32_t*>(ctx.r15);
                    if (mgs4e::mem::Readable(desc, 32) && desc[0x1c / 4] == 480 && desc[0x18 / 4] == 272)
                    {
                        ctx.rdi = static_cast<uint32_t>(g_SceneW);
                        ctx.rbx = static_cast<uint32_t>(g_SceneH);
                    }
                });
                spdlog::info("PW internal size: full-canvas scene targets created at {}x{} ({}).", g_SceneW, g_SceneH, g_SceneCreate ? "ok" : "hook FAILED");
            }
            else { spdlog::warn("PW internal size: scene creation site does not look as expected; the full-canvas targets keep the game's size."); }
        }
        if (canvasActive && iRenderScale > 0) { Canvas::Configure(canvasW, canvasH, iRenderScale); }
        {
            const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
            const uint8_t* get = reinterpret_cast<const uint8_t*>(base + kSettingsGet);
            if (kSettingsGet && kSettingsSet && get[0] == 0x48 && get[1] == 0x8B && get[2] == 0x05 && get[7] == 0x48 && get[8] == 0x63 && get[9] == 0xD1)
            {
                g_SettingsGet = safetyhook::create_inline(reinterpret_cast<void*>(base + kSettingsGet), reinterpret_cast<void*>(&Hooked_SettingsGet));
                g_SettingsSet = safetyhook::create_inline(reinterpret_cast<void*>(base + kSettingsSet), reinterpret_cast<void*>(&Hooked_SettingsSet));
                if (iWindowMode >= 0) { spdlog::info("PW window: display mode {} will replace the saved one ({}).", iWindowMode, g_SettingsGet ? "hooked" : "hook FAILED"); }
                else { spdlog::info("PW window: settings getter {} (saved values logged on first read).", g_SettingsGet ? "hooked" : "hook FAILED"); }
            }
            else { spdlog::warn("PW window: settings getter +{:X} does not look as expected; not hooked.", kSettingsGet); }
        }
        {
            // The picture fit is always hooked: it logs what the game computed (the window's
            // client size and the picture inside it), and it is overridden in two cases: the
            // back-buffer experiment (picture = the whole buffer) and the wide canvas (the
            // game fits a 16:9 picture; the wide frame must be fitted at its own aspect).
            const auto site = reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kAfterFit;
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(site);
            if (kAfterFit && bytes[0] == 0xB9 && bytes[1] == 0x11)   // mov ecx, 0x11
            {
                g_AfterFit = safetyhook::create_mid(site, [](SafetyHookContext& ctx)
                {
                    auto* obj = reinterpret_cast<int32_t*>(ctx.rsi);
                    const int clientW = obj[0x2958 / 4], clientH = obj[0x295C / 4];
                    spdlog::info("PW window: fit: client {}x{}, game picture {}x{} at {},{}.", clientW, clientH, obj[0x2940 / 4], obj[0x2944 / 4], obj[0x2948 / 4], obj[0x294C / 4]);
                    if (bBackBufferAtInternal && g_PostW > 0 && g_PostH > 0)
                    {
                        obj[0x2940 / 4] = g_PostW; obj[0x2944 / 4] = g_PostH;
                        obj[0x2948 / 4] = 0; obj[0x294C / 4] = 0;
                    }
                    else if (g_CanvasW != 480 || g_CanvasH != 272)
                    {
                        // The game fits a 480x272-shaped picture; the wide canvas needs its own
                        // aspect fitted into the window.
                        // The window is still at its start-up size when this runs (the chain is
                        // then created at the picture size), so the box is the output size, which
                        // is the window size the user chose.
                        int cw = iOutputWidth, ch = iOutputHeight;
                        if (cw > 0 && ch > 0)
                        {
                            const double aspect = static_cast<double>(g_CanvasW) / static_cast<double>(g_CanvasH);
                            int w = cw, h = static_cast<int>(cw / aspect + 0.5);
                            if (h > ch) { h = ch; w = static_cast<int>(ch * aspect + 0.5); }
                            obj[0x2940 / 4] = w; obj[0x2944 / 4] = h;
                            obj[0x2948 / 4] = (cw - w) / 2; obj[0x294C / 4] = (ch - h) / 2;
                            spdlog::info("PW window: fit overridden for the {}x{}-unit canvas: box {}x{}, picture {}x{} at {},{}.", g_CanvasW, g_CanvasH, cw, ch, w, h, obj[0x2948 / 4], obj[0x294C / 4]);
                        }
                    }
                });
                if (bBackBufferAtInternal) { spdlog::info("PW internal size: picture fit overridden to the whole {}x{} back buffer ({}).", g_PostW, g_PostH, g_AfterFit ? "hooked" : "hook FAILED"); }
            }
            else { spdlog::warn("PW internal size: fit site +{:X} does not look as expected; not hooked.", kAfterFit); }
        }
        if (bFullscreenResolutionFix)
        {
            const auto site = reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kAfterModeLoop;
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(site);
            if (kAfterModeLoop && bytes[0] == 0x44 && bytes[1] == 0x8B && bytes[2] == 0x4D && bytes[3] == 0xA8 && bytes[4] == 0x33 && bytes[5] == 0xD2)
            {
                g_AfterModeLoop = safetyhook::create_mid(site, [](SafetyHookContext& ctx)
                {
                    const int largestW = static_cast<int32_t>(ctx.r15), largestH = static_cast<int32_t>(ctx.r14);
                    int w = iOutputWidth, h = iOutputHeight;
                    if (w <= 0 || h <= 0)
                    {
                        DEVMODEW dm {};
                        dm.dmSize = sizeof(dm);
                        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) { w = static_cast<int>(dm.dmPelsWidth); h = static_cast<int>(dm.dmPelsHeight); }
                    }
                    if (w <= 0 || h <= 0) { return; }
                    ctx.r15 = static_cast<uint32_t>(w);
                    ctx.r14 = static_cast<uint32_t>(h);
                    spdlog::info("PW window: fullscreen mode {}x{} ({}) replaces the monitor's largest mode {}x{}.", w, h, iOutputWidth > 0 ? "the screen resolution" : "the desktop mode", largestW, largestH);
                });
                if (!g_AfterModeLoop) { spdlog::warn("PW window: fullscreen mode hook FAILED; the game keeps the monitor's largest mode."); }
            }
            else { spdlog::warn("PW window: fullscreen mode site +{:X} does not look as expected; not hooked.", kAfterModeLoop); }
        }
        if (iOutputWidth > 0 && iOutputHeight > 0)
        {
            const auto table = reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kOutputTable;
            for (int i = 0; i < kOutputSlots; i++)
            {
                mgs4e::mem::Poke<int32_t>(table + i * 8, iOutputWidth);
                mgs4e::mem::Poke<int32_t>(table + i * 8 + 4, iOutputHeight);
            }
            spdlog::info("PW internal size: output table ({} slots) set to {}x{}.", kOutputSlots, iOutputWidth, iOutputHeight);
        }
    }
}


