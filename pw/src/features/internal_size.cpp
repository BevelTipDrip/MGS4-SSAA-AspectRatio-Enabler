#include "pch.hpp"
#include "internal_size.hpp"
#include <atomic>
#include <cmath>

#include "game.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "ui_bias.hpp"

#if MGS4E_LAB_BUILD

namespace
{
    // Every site that turns the -resolution index into the internal size carries both sizes as
    // immediates (the "1" branch first, then the "0" branch). Offsets are of the immediate
    // itself, measured on the live dump of 2026-09-13 (build 1.3.1.0).
    struct Imm32Site { uintptr_t rva; uint32_t expected; bool isWidth; };
    // Getter 1 (+16201) is read by the function that creates the post-chain targets (the
    // resolved colour, the HDR target, the post depth: creation stacks +1642D/+1677C/+16801/
    // +16A38 under +23200); it takes the post size. The other sites size and place the scene
    // (HUD scale, resolve, viewport getters) and take the scene size.
    constexpr Imm32Site kPostImm32[] = {
        { 0x1620E, 0x780, true }, { 0x16214, 0x440, false }, { 0x1621C, 0x5A0, true }, { 0x16222, 0x330, false },   // target getter 1
    };
    constexpr Imm32Site kImm32[] = {
        { 0x1AC4B, 0x780, true }, { 0x1AC51, 0x440, false }, { 0x1AC59, 0x5A0, true }, { 0x1AC5F, 0x330, false },   // target getter 3
        { 0x51ECA, 0x780, true }, { 0x51ECF, 0x440, false }, { 0x51ED6, 0x5A0, true }, { 0x51EDB, 0x330, false },   // target getter 2
        { 0x56306, 0x780, true }, { 0x56327, 0x5A0, true },                                                         // HUD scale (width only)
    };
    struct Imm64Site { uintptr_t rva; uint64_t expected; };
    constexpr Imm64Site kImm64[] = {
        { 0x3F509, 0x44000000780ull }, { 0x3F51D, 0x330000005A0ull },   // packed getter
        { 0x5C22C, 0x44000000780ull }, { 0x5C245, 0x330000005A0ull },   // internal target creation
    };
    // The render-scale getter (+3F530): returns (scaleY << 32 | scaleX) as an immediate per
    // -resolution value; every scene target is the 480x272 canvas times it (4x MSAA on the
    // scene colour target), so it must stay an integer.
    constexpr Imm64Site kScale[] = { { 0x3F539, 0x400000004ull }, { 0x3F54D, 0x300000003ull } };
    constexpr uintptr_t kOutputTable = 0xD8F1C8;   // four (width, height) int pairs, .rdata
    constexpr int kOutputSlots = 4;
    // Just after the game's picture fit (+1A2B5..+1A360) has written the picture size and
    // offsets into the display object (rsi): picture w/h at +0x2940/+0x2944, x/y offset at
    // +0x2948/+0x294c. With the back buffer at the internal size the picture is the whole buffer.
    constexpr uintptr_t kAfterFit = 0x1A366;   // mov ecx, 0x11
    SafetyHookMid g_AfterFit {};
    // The two sites that turn the integer scale into a float: the game scaler (+596E5,
    // `cvtsi2ss xmm1, rcx` after the getter; replaced by our value, the instruction skipped) and
    // the render-scale store (+5B015, `addss xmm0, 0.5` then truncation; xmm0 replaced first).
    // Forcing a fraction here reproduces the fractional scale of Afevis's patch on purpose.
    constexpr uintptr_t kGameScaler = 0x596E5;
    constexpr uintptr_t kRenderScaleStore = 0x5B015;
    SafetyHookMid g_GameScaler {}, g_RenderScaleStore {};
    // Scene target creation (+89F8D): edi = scale x canvas width, ebx = scale x canvas height,
    // the descriptor in r15 (+0x1c width, +0x18 height in canvas units). Afevis replaces the
    // full-canvas targets' size here with the output size; the experiment does the same.
    constexpr uintptr_t kSceneCreate = 0x89F8D;
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
    constexpr uintptr_t kSettingsGet = 0x84D50;
    constexpr uintptr_t kSettingsSet = 0x85130;
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
    int g_SceneW = 0, g_SceneH = 0;
    int g_CanvasW = 480, g_CanvasH = 272;   // the canvas in PSP units for this run (the fit hook reads it)
    float g_FloatScale = 0;

    template <typename T>
    bool PatchChecked(uintptr_t rva, T expected, T value, const char* what)
    {
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
    int BackBufferWidth() { return bBackBufferAtInternal ? iInternalWidth : 0; }
    int BackBufferHeight() { return bBackBufferAtInternal ? iInternalHeight : 0; }

    void Apply()
    {
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
            const bool wider = aspect > 480.0 / 272.0;
            canvasW = iWideCanvasUnits > 0 ? iWideCanvasUnits : (wider ? static_cast<int>(std::lround(272.0 * aspect)) : 480);
            canvasH = iWideCanvasHeightUnits > 0 ? iWideCanvasHeightUnits : (wider ? 272 : static_cast<int>(std::lround(480.0 / aspect)));
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
        if (canvasActive && iRenderScale > 0 && iSceneWidth == 0 && iSceneHeight == 0)
        {
            iSceneWidth = sceneW; iSceneHeight = sceneH;   // the full-canvas targets must follow
        }
        // Post size: the explicit internal size when given together with a render scale, else the scene size.
        const int postW = (iRenderScale > 0 && iInternalWidth > 0) ? iInternalWidth : sceneW;
        const int postH = (iRenderScale > 0 && iInternalHeight > 0) ? iInternalHeight : sceneH;
        if (sceneW > 0 && sceneH > 0)
        {
            int ok = 0;
            for (const Imm32Site& s : kImm32)
            {
                const bool hudSite = (s.rva == 0x56306 || s.rva == 0x56327);
                ok += PatchChecked<uint32_t>(s.rva, s.expected, static_cast<uint32_t>(s.isWidth ? (hudSite ? hudW : sceneW) : sceneH), "scene size immediate");
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
            iInternalWidth = postW; iInternalHeight = postH;
        }
        if (iRenderScaleHundredths > 0)
        {
            g_FloatScale = static_cast<float>(iRenderScaleHundredths) / 100.0f;
            const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
            const uint8_t* a = reinterpret_cast<const uint8_t*>(base + kGameScaler);
            const uint8_t* b = reinterpret_cast<const uint8_t*>(base + kRenderScaleStore);
            if (a[0] == 0xF3 && a[1] == 0x48 && a[2] == 0x0F && a[3] == 0x2A && b[0] == 0xF3 && b[1] == 0x0F && b[2] == 0x58)
            {
                g_GameScaler = safetyhook::create_mid(base + kGameScaler, [](SafetyHookContext& ctx) { ctx.xmm1.f32[0] = g_FloatScale; ctx.rip += 5; });
                g_RenderScaleStore = safetyhook::create_mid(base + kRenderScaleStore, [](SafetyHookContext& ctx) { ctx.xmm0.f32[0] = g_FloatScale; });
                spdlog::info("PW internal size: EXPERIMENT: float render scale forced to {:.2f} at the game scaler and the render-scale store ({}/{}).", g_FloatScale, g_GameScaler ? "ok" : "FAILED", g_RenderScaleStore ? "ok" : "FAILED");
            }
            else { spdlog::warn("PW internal size: float scale sites do not look as expected; experiment not installed."); }
        }
        if (iSceneWidth > 0 && iSceneHeight > 0)
        {
            g_SceneW = iSceneWidth; g_SceneH = iSceneHeight;
            const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
            const uint8_t* c = reinterpret_cast<const uint8_t*>(base + kSceneCreate);
            if (c[0] == 0xE8)   // call +17D20
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
                spdlog::info("PW internal size: EXPERIMENT: full-canvas scene targets created at {}x{} ({}).", g_SceneW, g_SceneH, g_SceneCreate ? "ok" : "hook FAILED");
            }
            else { spdlog::warn("PW internal size: scene creation site does not look as expected; experiment not installed."); }
        }
        if (canvasActive && iRenderScale > 0) { UiBias::ConfigureWideScene(canvasW, canvasH, iRenderScale); }
        {
            const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
            const uint8_t* get = reinterpret_cast<const uint8_t*>(base + kSettingsGet);
            if (get[0] == 0x48 && get[1] == 0x8B && get[2] == 0x05 && get[7] == 0x48 && get[8] == 0x63 && get[9] == 0xD1)
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
            if (bytes[0] == 0xB9 && bytes[1] == 0x11)   // mov ecx, 0x11
            {
                g_AfterFit = safetyhook::create_mid(site, [](SafetyHookContext& ctx)
                {
                    auto* obj = reinterpret_cast<int32_t*>(ctx.rsi);
                    const int clientW = obj[0x2958 / 4], clientH = obj[0x295C / 4];
                    spdlog::info("PW window: fit: client {}x{}, game picture {}x{} at {},{}.", clientW, clientH, obj[0x2940 / 4], obj[0x2944 / 4], obj[0x2948 / 4], obj[0x294C / 4]);
                    if (bBackBufferAtInternal && iInternalWidth > 0 && iInternalHeight > 0)
                    {
                        obj[0x2940 / 4] = iInternalWidth; obj[0x2944 / 4] = iInternalHeight;
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
                if (bBackBufferAtInternal) { spdlog::info("PW internal size: picture fit overridden to the whole {}x{} back buffer ({}).", iInternalWidth, iInternalHeight, g_AfterFit ? "hooked" : "hook FAILED"); }
            }
            else { spdlog::warn("PW internal size: fit site +{:X} does not look as expected; not hooked.", kAfterFit); }
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

#endif
