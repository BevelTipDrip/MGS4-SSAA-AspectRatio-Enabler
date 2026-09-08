#include "pch.hpp"

#include "graphics_settings.hpp"

#include "game.hpp"
#include "mem.hpp"
#include "log.hpp"
#include "render_pipeline.hpp"
#include "compat.hpp"
#include "settings_keys.hpp"

#include <Zydis/Zydis.h>

namespace
{
    // The engine parses its render config into these globals at startup; the reticle
    // conversions below are scaled against them, which is what identifies those
    // conversions as screen coordinates rather than any other narrowing.
    constexpr uintptr_t kRvaRenderBufferSizeX = 0x1B00000;
    constexpr uintptr_t kRvaRenderBufferSizeY = 0x1B00004;

    // ======================= The reticle truncation fix =======================
    //
    // The engine converts a screen coordinate into its 1280x720 virtual UI space like this:
    //
    //   cvttss2si ecx, xmm0        ; float -> int32, full precision in ECX
    //   movsx     edx, cx          ; keep only the low 16 bits, sign-extended  <-- the defect
    //   lea       eax, [rdx+rdx*4] ; x5
    //   shl       eax, 8           ; x256, so x1280 in total
    //   idiv      [render.bufferSizeX]
    //
    // The Y routes are the same shape with `imul eax, r, 0x2D0` (x720) against bufferSizeY. The
    // 1280 and 720 are the same virtual basis measured at runtime, where the reticle quads carry
    // v6.x = 2/1280 and v7.y = -2/720.
    //
    // The truncation is what loses the reticle: at a 7680-wide buffer the centre is 3840px,
    // which is 61440 in 1/16px units, and movsx edx, cx turns that into -4096 - exactly the
    // coordinate measured coming back out of the vertex stream. Keeping the full 32 bits removes
    // the 4095-wide ceiling at its source.
    //
    // The same shape, in other registers, positions the Solid Eye's world-pinned markers
    // (+14CF68E/+14D3042: `cvttss2si eax, xmm0` then `movsx ecx, ax`, scaled and divided the
    // same way). Those markers do not vanish when they wrap - they land somewhere else on
    // screen: an enemy in front of Snake gets its targeting icon at the far left, because
    // 3410px * 16 = 54560 reads back as -10976. The scan is therefore keyed on the shape, not
    // on which registers the compiler chose.
    //
    // The scan, encoding and verification below are our own: rather than hardcoding addresses it
    // looks for the shape of the defect - a float-to-int conversion narrowed to 16 bits and then
    // scaled against a render size global - so it survives the code shifting and refuses cleanly
    // if the pattern is ever absent.

    // Whether a site still holds the game's narrowing conversion, or our widened one.
    //
    // Distinguishing these matters for the message as much as the behaviour: without it, a
    // second pass over already-patched code finds no narrowing conversions and reports that the
    // game build differs, which is both wrong and alarming.
    enum class SiteState
    {
        Narrowing,      // movsx r32, r16 - the original defect, ours to patch
        Widened,        // mov r32, r32   - already patched, nothing to do
    };

    struct TruncationSite
    {
        uintptr_t rva;
        size_t length;                  // bytes occupied by the conversion
        std::array<uint8_t, 4> original;      // what is there now, verified again before writing
        std::array<uint8_t, 4> replacement;
        ZydisRegister destination;
        ZydisRegister source;           // the cvttss2si destination, holding the full value
        const char* axis;
        SiteState state;
    };

    bool IsGpr32(ZydisRegister reg)
    {
        return reg >= ZYDIS_REGISTER_EAX && reg <= ZYDIS_REGISTER_R15D;
    }

    // The 16-bit view of a 32-bit register: eax -> ax, r8d -> r8w. Zydis numbers both sets in
    // hardware order, so the index carries over.
    ZydisRegister LowHalf(ZydisRegister gpr32)
    {
        return static_cast<ZydisRegister>(ZYDIS_REGISTER_AX + (gpr32 - ZYDIS_REGISTER_EAX));
    }

    // Builds `mov dest, source` to replace `movsx dest, source16`, padded with NOPs to the
    // original length.
    //
    // Opcode 8B /r is MOV r32, r/m32; with a mod=11 ModRM the byte is 0xC0 | (dest << 3) | src.
    // r8d-r15d on either side need a REX prefix (R for the destination, B for the source),
    // which is why those encodings are a byte longer.
    bool BuildFullWidthMove(ZydisRegister destination, ZydisRegister source, size_t length,
        std::array<uint8_t, 4>& out)
    {
        if (!IsGpr32(destination) || !IsGpr32(source) || length < 2 || length > out.size())
        {
            return false;
        }

        const int destIndex = destination - ZYDIS_REGISTER_EAX;
        const int srcIndex = source - ZYDIS_REGISTER_EAX;
        const uint8_t modrm = static_cast<uint8_t>(
            0xC0 | ((destIndex & 7) << 3) | (srcIndex & 7));

        uint8_t rex = 0x40;
        if (destIndex >= 8) { rex |= 0x04; }                                    // REX.R
        if (srcIndex >= 8) { rex |= 0x01; }                                     // REX.B

        out.fill(0x90);                                                          // nop padding

        size_t written = 0;
        if (rex != 0x40)
        {
            out[written++] = rex;
        }
        out[written++] = 0x8B;
        out[written++] = modrm;

        return written <= length;
    }

    // Every place a converted coordinate is narrowed to 16 bits and then scaled by a render size
    // global. Anchored on cvttss2si and decoded forward with Zydis rather than matched as a byte
    // string, so the surrounding code is free to differ between builds.
    std::vector<TruncationSite> FindTruncationSites()
    {
        std::vector<TruncationSite> sites;

        // Bounded to .text, and with no per-byte readability check.
        //
        // Scanning SizeOfImage with a VirtualQuery per byte is a documented way to hang this
        // game - 605MB of one-byte probes - and doing it here stalled startup badly enough that
        // the game never finished loading. The section header gives the executable range
        // directly, and it is committed for the life of the process.
        const auto imageBase = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mgs4e::game::Module());
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const uint8_t*>(mgs4e::game::Module()) + dos->e_lfanew);

        uint8_t* textStart = nullptr;
        size_t textSize = 0;

        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++)
        {
            if (std::memcmp(section->Name, ".text", 5) == 0)
            {
                textStart = reinterpret_cast<uint8_t*>(imageBase) + section->VirtualAddress;
                textSize = section->Misc.VirtualSize;
                break;
            }
        }

        if (!textStart || textSize == 0)
        {
            return sites;
        }

        const size_t textRva = static_cast<size_t>(textStart - reinterpret_cast<uint8_t*>(imageBase));

        const uintptr_t widthGlobal = imageBase + kRvaRenderBufferSizeX;
        const uintptr_t heightGlobal = imageBase + kRvaRenderBufferSizeY;

        ZydisDecoder decoder;
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

        // cvttss2si r32, xmm - the conversion producing the full-precision coordinate. The
        // opcode bytes are a cheap prefilter (an optional REX sits between F3 and 0F); the
        // decoder then names the register the value lands in, which is what the narrowing has
        // to read from. The reticle routes use ecx, the Solid Eye marker routes eax.
        constexpr std::array<uint8_t, 2> kConvertOpcode = { 0x0F, 0x2C };

        for (size_t at = 0; at + 64 < textSize; at++)
        {
            if (textStart[at] != 0xF3)
            {
                continue;
            }

            size_t opcodeAt = at + 1;
            if ((textStart[opcodeAt] & 0xF0) == 0x40)   // REX
            {
                opcodeAt++;
            }
            if (std::memcmp(textStart + opcodeAt, kConvertOpcode.data(), kConvertOpcode.size()) != 0)
            {
                continue;
            }

            ZydisDecodedInstruction convert;
            ZydisDecodedOperand convertOperands[ZYDIS_MAX_OPERAND_COUNT];
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, textStart + at, textSize - at,
                &convert, convertOperands))
                || convert.mnemonic != ZYDIS_MNEMONIC_CVTTSS2SI
                || convertOperands[0].type != ZYDIS_OPERAND_TYPE_REGISTER
                || !IsGpr32(convertOperands[0].reg.value))
            {
                continue;
            }

            const ZydisRegister source = convertOperands[0].reg.value;
            const ZydisRegister source16 = LowHalf(source);

            // Walk forward a short way for the narrowing, then for the render size it is scaled
            // against. Both must be present - a movsx on its own is ordinary code.
            size_t offset = at + convert.length;
            size_t movsxOffset = 0;
            size_t movsxLength = 0;
            ZydisRegister destination = ZYDIS_REGISTER_NONE;
            SiteState foundState = SiteState::Narrowing;

            for (int step = 0; step < 10 && offset + 16 < textSize; step++)
            {
                ZydisDecodedInstruction instruction;
                ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, textStart + offset,
                    textSize - offset, &instruction, operands)))
                {
                    break;
                }

                // Either shape counts as a hit: the narrowing we are looking for, or the
                // widened form we may have written on an earlier pass. `mov r32, r32` is a very
                // common instruction on its own, which is why it is only ever accepted here -
                // between a cvttss2si and a scale against a render size global - and is used to
                // classify rather than to decide what to overwrite.
                const bool isNarrowing =
                    instruction.mnemonic == ZYDIS_MNEMONIC_MOVSX
                    && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER
                    && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER
                    && operands[1].reg.value == source16
                    && IsGpr32(operands[0].reg.value);

                const bool isWidened =
                    instruction.mnemonic == ZYDIS_MNEMONIC_MOV
                    && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER
                    && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER
                    && operands[1].reg.value == source
                    && IsGpr32(operands[0].reg.value);

                // A division before any narrowing means the conversion was consumed at full
                // width: what follows is the quotient being copied out (`mov r14d, eax`) ahead of
                // the *next* conversion's scale, which the widened-form test would otherwise
                // mistake for an earlier pass of ours (three such runs in the Solid Eye code).
                if (movsxOffset == 0 && instruction.mnemonic == ZYDIS_MNEMONIC_IDIV)
                {
                    break;
                }

                if (movsxOffset == 0 && (isNarrowing || isWidened))
                {
                    movsxOffset = offset;
                    movsxLength = instruction.length;
                    destination = operands[0].reg.value;
                    foundState = isNarrowing ? SiteState::Narrowing : SiteState::Widened;

                    // A widened site is followed by the NOP padding we wrote, so take the whole
                    // run - otherwise the recorded length is short and the bytes will not match.
                    if (isWidened)
                    {
                        size_t padding = offset + instruction.length;
                        while (padding < textSize && movsxLength < 4
                            && textStart[padding] == 0x90)
                        {
                            movsxLength++;
                            padding++;
                        }
                    }
                }
                else if (movsxOffset != 0)
                {
                    for (ZyanU8 i = 0; i < instruction.operand_count; i++)
                    {
                        if (operands[i].type != ZYDIS_OPERAND_TYPE_MEMORY
                            || operands[i].mem.base != ZYDIS_REGISTER_RIP)
                        {
                            continue;
                        }

                        const auto target = reinterpret_cast<uintptr_t>(textStart + offset)
                            + instruction.length
                            + static_cast<uintptr_t>(operands[i].mem.disp.value);

                        if (target != widthGlobal && target != heightGlobal)
                        {
                            continue;
                        }

                        TruncationSite site {};
                        site.rva = textRva + movsxOffset;
                        site.length = movsxLength;
                        site.destination = destination;
                        site.source = source;
                        site.axis = (target == widthGlobal) ? "X" : "Y";
                        site.state = foundState;
                        site.original.fill(0);
                        std::memcpy(site.original.data(), textStart + movsxOffset, movsxLength);

                        if (BuildFullWidthMove(destination, source, movsxLength, site.replacement))
                        {
                            sites.push_back(site);
                        }

                        movsxOffset = 0;
                        break;
                    }
                }

                offset += instruction.length;
            }
        }

        return sites;
    }

    void PatchReticleTruncation()
    {
        if (!RenderPipeline::bFixReticleTruncation)
        {
            return;
        }

        // .text is encrypted on disk and decrypted by the Steam wrapper, so the instructions may
        // not be readable yet. Wait for them rather than concluding the game has changed.
        std::vector<TruncationSite> sites;
        for (int attempt = 0; attempt < 100; attempt++)
        {
            sites = FindTruncationSites();
            if (!sites.empty())
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        if (sites.empty())
        {
            spdlog::warn("MGS4: Reticle truncation: no conversion of the expected shape found. "
                "The game build differs from the one this was written against; nothing patched.");
            return;
        }

        // Refuse on a mixed state rather than patching the remainder.
        //
        // Half-patched code means something other than this function has been writing here - a
        // second injector, or a partially failed pass - and completing the job on that
        // assumption is how you corrupt an instruction stream.
        const auto narrowing = std::count_if(sites.begin(), sites.end(),
            [](const TruncationSite& site) { return site.state == SiteState::Narrowing; });
        const auto widened = static_cast<ptrdiff_t>(sites.size()) - narrowing;

        if (narrowing == 0)
        {
            spdlog::info("MGS4: Reticle truncation: all {} conversion(s) are already widened.",
                sites.size());
            return;
        }

        if (widened != 0)
        {
            spdlog::error("MGS4: Reticle truncation: {} conversion(s) still narrow and {} already "
                "widened. A partially patched state is not ours to complete; nothing written.",
                narrowing, widened);
            return;
        }

        int patched = 0;
        for (const auto& site : sites)
        {
            auto* const at = reinterpret_cast<uint8_t*>(
                reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + site.rva);

            // Verify the bytes have not changed since the scan. The scan and this write are
            // separated by decoding work, and .text is being decrypted underneath us, so
            // confirming immediately before writing is what makes the shape match safe to act
            // on - it is the guarantee a hardcoded byte table would have given for free.
            if (std::memcmp(at, site.original.data(), site.length) != 0)
            {
                spdlog::error("MGS4: Reticle truncation: +{:X} changed between scan and write; "
                    "skipped.", site.rva);
                continue;
            }

            DWORD previous = 0;
            if (!VirtualProtect(at, site.length, PAGE_EXECUTE_READWRITE, &previous))
            {
                spdlog::error("MGS4: Reticle truncation: could not unprotect +{:X}.", site.rva);
                continue;
            }

            std::memcpy(at, site.replacement.data(), site.length);
            FlushInstructionCache(GetCurrentProcess(), at, site.length);
            VirtualProtect(at, site.length, previous, &previous);

            // And confirm it landed, so a silently failed write cannot be reported as success.
            if (std::memcmp(at, site.replacement.data(), site.length) != 0)
            {
                spdlog::error("MGS4: Reticle truncation: +{:X} did not verify after writing.",
                    site.rva);
                continue;
            }

            patched++;
            spdlog::info("MGS4: Reticle truncation: +{:X} movsx {}, {} widened to 32-bit ({})",
                site.rva, ZydisRegisterGetString(site.destination),
                ZydisRegisterGetString(LowHalf(site.source)), site.axis);
        }

        if (patched != static_cast<int>(sites.size()))
        {
            spdlog::error("MGS4: Reticle truncation: only {} of {} conversion(s) were widened. "
                "The reticle or the Solid Eye markers may still be lost at high resolutions.",
                patched, sites.size());
            return;
        }

        spdlog::info("MGS4: Reticle truncation: {} conversion(s) widened - the reticle and the "
            "Solid Eye markers keep full precision, so buffers wider than 4095 no longer lose "
            "them.", patched);
    }

}

void GraphicsSettings::PatchReticleTruncation()
{
    ::PatchReticleTruncation();
}

void GraphicsSettings::ApplyShadowAndAntiAliasing()
{
    using namespace RenderPipeline;

        // The engine's scalability config supplies "ShadowBufferSize" (group 5), parsed
        // here and stored before anything is allocated, so scaling it at this point
        // raises the shadow map everywhere the engine derives from it:
        //   TEST DIL,DIL              ; did the key match "ShadowBufferSize"?
        //   JZ   <skip>
        //   LEA  RCX,[RBX + 0x30]
        //   CALL <parse int>          ; -> EAX
        //   MOV  [RBP-0x60], 5        ; group 5, and where we hook
        //   MOV  [RBP-0x50], EAX      ; the value gets stored here
        // The group id in the instruction discriminates this from the otherwise identical
        // ShadowSampleCount (group 4) branch immediately above it.
        if (fShadowResolutionScale > 1.0
            && !mgs4e::compat::Yields(mgs4e::keys::Graphics, mgs4e::keys::ShadowResolutionScale))
        {
            if (uint8_t* ShadowBufferSizeResult = mgs4e::mem::FindPattern(mgs4e::game::Module(),
                "48 8D 4B 30 E8 ?? ?? ?? ?? C7 45 ?? 05 00 00 00 49 8B 55 08",
                "MGS4: Shadow Resolution"))
            {
                static SafetyHookMid ShadowResolutionMidHook {};
                ShadowResolutionMidHook = safetyhook::create_mid(ShadowBufferSizeResult + 0x9,
                    [](SafetyHookContext& ctx)
                    {
                        const auto base = static_cast<int32_t>(ctx.rax & 0xFFFFFFFF);
                        if (base <= 0)
                        {
                            return;
                        }

                        int target = static_cast<int>(std::lround(base * fShadowResolutionScale));
                        target &= ~127; // keep it a tidy multiple of 128

                        // The atlas is allocated as width x 2*width, and D3D12 caps a 2D
                        // texture at 16384, so the width itself can't exceed 8192.
                        constexpr int kMaxShadowBufferSize = 8192;
                        if (target > kMaxShadowBufferSize)
                        {
                            spdlog::warn("MGS4: Shadow Resolution: {} exceeds the safe limit of {}, clamping.",
                                target, kMaxShadowBufferSize);
                            target = kMaxShadowBufferSize;
                        }

                        if (target <= base)
                        {
                            return;
                        }

                        spdlog::info("MGS4: Shadow Resolution: ShadowBufferSize {} -> {} (atlas {}x{} -> {}x{}).",
                            base, target, base, base * 2, target, target * 2);

                        ctx.rax = (ctx.rax & ~0xFFFFFFFFull) | static_cast<uint32_t>(target);
                    });
                MGS4E_LOG_HOOK(ShadowResolutionMidHook, "MGS4: Shadow Resolution")
            }
        }

        // FXAA. The engine reads it through a getter that builds the key from "render.fxaa"
        // plus a platform suffix (fxaa_PS5, fxaa_Switch_Docked and so on), so there is no
        // single global to write - but the getter is specific to this setting, and by the end
        // of it the answer is in EAX:
        //   MOVZX EAX,BL      ; the parsed value
        //   MOV   RBX,[RSP+0x60]   <- hooked here, EAX already set
        //   ...
        //   RET
        // The game exposes FXAA in its own menu, but only as an on/off that needs a restart.
        if (iFxaaOverride >= 0)
        {
            if (uint8_t* FxaaResult = mgs4e::mem::FindPattern(mgs4e::game::Module(),
                "0F B6 C3 48 8B 5C 24 ?? 48 8B 7C 24 ?? 48 83 C4 50 5D C3",
                "MGS4: FXAA"))
            {
                static SafetyHookMid FxaaMidHook {};
                FxaaMidHook = safetyhook::create_mid(FxaaResult + 0x3,
                    [](SafetyHookContext& ctx)
                    {
                        const auto current = static_cast<uint32_t>(ctx.rax & 0xFFFFFFFF);
                        const auto wanted = static_cast<uint32_t>(iFxaaOverride ? 1 : 0);
                        if (current == wanted)
                        {
                            return;
                        }

                        static std::atomic<int> reported { 0 };
                        if (reported.fetch_add(1) < 2)
                        {
                            spdlog::info("MGS4: FXAA: {} -> {}.",
                                current ? "on" : "off", wanted ? "on" : "off");
                        }
                        ctx.rax = (ctx.rax & ~0xFFFFFFFFull) | wanted;
                    });
                MGS4E_LOG_HOOK(FxaaMidHook, "MGS4: FXAA")
            }
        }

        // FXAA quality, which the game exposes nowhere. Parsed as a double, narrowed to float
        // and stored:
        //   CVTSD2SS XMM1,XMM0
        //   MOVSS    [global],XMM1   <- hooked here, so XMM1 is the value about to be stored
        // The config documents it as Slow(0), Medium(1), Fast(2) and ships 1, so 0 is the
        // best-looking setting rather than the worst.
        if (iFxaaQuality >= 0)
        {
            if (uint8_t* FxaaParamResult = mgs4e::mem::FindPattern(mgs4e::game::Module(),
                "0F 5A C8 F3 0F 11 0D ?? ?? ?? ?? 48 83 C4 28 C3",
                "MGS4: FXAA Quality"))
            {
                static SafetyHookMid FxaaQualityMidHook {};
                FxaaQualityMidHook = safetyhook::create_mid(FxaaParamResult + 0x3,
                    [](SafetyHookContext& ctx)
                    {
                        const float current = ctx.xmm1.f32[0];
                        const auto wanted = static_cast<float>(iFxaaQuality);
                        if (current == wanted)
                        {
                            return;
                        }

                        // The game re-stores this every frame, so log the override only when
                        // the incoming value changes - and a few times at most, as a guard
                        // against a value that oscillates.
                        static std::atomic<float> lastSeen { -1.0f };
                        static std::atomic<int> reported { 0 };
                        if (lastSeen.exchange(current) != current && reported.fetch_add(1) < 4)
                        {
                            spdlog::info("MGS4: FXAA Quality: {} -> {}.", current, wanted);
                        }
                        ctx.xmm1.f32[0] = wanted;
                    });
                MGS4E_LOG_HOOK(FxaaQualityMidHook, "MGS4: FXAA Quality")
            }
        }

        // The ShadowSampleCount branch immediately above (group 4) was hooked by "Shadow
        // Softness (Samples)" until 0.0.6. The value reaches the shaders as the uniform
        // vts_shadowSampleCount (cb0[30].x), but the game's filter is a centre tap and a ring
        // of (count - 1) taps at 0.7 of one shadow texel (vts_shadowbufferSize, cb0[29], set
        // by the engine to 1/W and 1/2W of the W x 2W atlas), so more taps never widened it,
        // and a higher Shadow Resolution Scale makes the edge harder, not softer. Softening
        // is that texel uniform, which FusionFix's ShadowTexelOverride sets (its hook on the
        // setter at +663FA0 writes the value into XMM1/XMM2; its upload hook pins the count
        // to 8 for its shaders). Left to FusionFix, on its tab.
}
