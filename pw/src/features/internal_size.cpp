#include "pch.hpp"
#include "internal_size.hpp"

#include "game.hpp"
#include "log.hpp"
#include "mem.hpp"

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
        if (iRenderScale > 0)
        {
            const uint64_t packed = (static_cast<uint64_t>(iRenderScale) << 32) | static_cast<uint32_t>(iRenderScale);
            int ok = 0;
            for (const Imm64Site& s : kScale) { ok += PatchChecked<uint64_t>(s.rva, s.expected, packed, "render scale immediate"); }
            spdlog::info("PW internal size: render scale set to {} at {} of {} sites.", iRenderScale, ok, std::size(kScale));
        }
        // Scene size: the render scale's canvas multiple when set, else the explicit internal size.
        const int sceneW = iRenderScale > 0 ? 480 * iRenderScale : iInternalWidth;
        const int sceneH = iRenderScale > 0 ? 272 * iRenderScale : iInternalHeight;
        // Post size: the explicit internal size when given together with a render scale, else the scene size.
        const int postW = (iRenderScale > 0 && iInternalWidth > 0) ? iInternalWidth : sceneW;
        const int postH = (iRenderScale > 0 && iInternalHeight > 0) ? iInternalHeight : sceneH;
        if (sceneW > 0 && sceneH > 0)
        {
            int ok = 0;
            for (const Imm32Site& s : kImm32)
            {
                ok += PatchChecked<uint32_t>(s.rva, s.expected, static_cast<uint32_t>(s.isWidth ? sceneW : sceneH), "scene size immediate");
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
            spdlog::info("PW internal size: scene {}x{}, post chain {}x{}, at {} of {} sites.", sceneW, sceneH, postW, postH, ok, std::size(kImm32) + std::size(kImm64) + std::size(kPostImm32));
            iInternalWidth = postW; iInternalHeight = postH;
        }
        if (bBackBufferAtInternal && iInternalWidth > 0 && iInternalHeight > 0)
        {
            const auto site = reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kAfterFit;
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(site);
            if (bytes[0] == 0xB9 && bytes[1] == 0x11)   // mov ecx, 0x11
            {
                g_AfterFit = safetyhook::create_mid(site, [](SafetyHookContext& ctx)
                {
                    auto* obj = reinterpret_cast<int32_t*>(ctx.rsi);
                    obj[0x2940 / 4] = iInternalWidth;
                    obj[0x2944 / 4] = iInternalHeight;
                    obj[0x2948 / 4] = 0;
                    obj[0x294C / 4] = 0;
                });
                spdlog::info("PW internal size: picture fit overridden to the whole {}x{} back buffer ({}).", iInternalWidth, iInternalHeight, g_AfterFit ? "hooked" : "hook FAILED");
            }
            else { spdlog::warn("PW internal size: fit site +{:X} does not look as expected; back buffer override not installed.", kAfterFit); }
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
