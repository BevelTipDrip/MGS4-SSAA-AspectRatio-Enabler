#include "stdafx.h"

#include "aspect_ratio.hpp"

#include "common.hpp"
#include "logging.hpp"

namespace AspectRatio
{
    // The UI layout converter. Runs once per layout object at creation, always with the full
    // logical canvas (0,0 1280x720), and writes the object's mapping: a physical viewport at
    // +0x200..0x20C (the full output size, whatever its aspect) and the logical range that
    // maps onto it at +0x210..0x21C (always 1280x720). That mapping is the entire cause of
    // the HUD stretching at non-16:9 outputs.
    //
    // The function and the object's field layout were identified by the mgs4Ultra120 project
    // (github.com/drbermejor/mgs4Ultra120); the call pattern and the written values were
    // measured here with the Log UI Layout diagnostic before anything was built on them.
    constexpr uintptr_t kRvaUiLayoutConverter = 0x439810;

    // The engine's output size globals, render.windowSizeX/Y. Same location the internal
    // resolution hook resolves by signature and logs at startup ("windowSize at +1B00460");
    // kept as constants here so this file stands alone if the hook is disabled.
    constexpr uintptr_t kRvaWindowSizeX = 0x1B00460;
    constexpr uintptr_t kRvaWindowSizeY = 0x1B00464;

    // Field offsets within a layout object. Logical range is left/top/right/bottom.
    constexpr uintptr_t kLayoutLogicalLeft = 0x210;
    constexpr uintptr_t kLayoutLogicalTop = 0x214;
    constexpr uintptr_t kLayoutLogicalRight = 0x218;
    constexpr uintptr_t kLayoutLogicalBottom = 0x21C;

    SafetyHookInline UiLayoutConverter_hook {};

    // Widen the logical range instead of shrinking the physical viewport.
    //
    // Both produce undistorted widgets, but shrinking the viewport would also clip anything
    // the UI draws outside the 16:9 area - and full-output overlays (fades, letterbox bars,
    // cinema grain) legitimately cover the whole frame. With a wider logical range the 0..1280
    // interval lands on a centered 16:9 area while the object can still address the full
    // output. mgs4Ultra120 reached the same conclusion for its centered HUD.
    //
    // The original runs first and its writes are then adjusted, rather than reimplementing
    // it: whatever else the function does to the object - the dirty flags at +0x18 are known,
    // there may be more - keeps happening.
    uint8_t ConvertLayout(uintptr_t object, int32_t x, int32_t y, int32_t width, int32_t height)
    {
        const uint8_t result = UiLayoutConverter_hook.stdcall<uint8_t>(object, x, y, width, height);

        // Only the full-canvas call is understood well enough to adjust. Nothing else has
        // ever been observed (86 of 86 calls in a full boot-to-gameplay flow), but if a
        // partial-canvas call exists somewhere, passing it through unmodified degrades to
        // the game's own behaviour rather than to a guess.
        if (!object || x != 0 || y != 0 || width != 1280 || height != 720)
        {
            return result;
        }

        int32_t outputWidth = 0;
        int32_t outputHeight = 0;
        std::memcpy(&outputWidth, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(baseModule) + kRvaWindowSizeX), sizeof(outputWidth));
        std::memcpy(&outputHeight, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(baseModule) + kRvaWindowSizeY), sizeof(outputHeight));

        if (outputWidth < 320 || outputHeight < 240 || outputWidth > 16384 || outputHeight > 16384)
        {
            return result;
        }

        const int64_t wide = static_cast<int64_t>(outputWidth) * 9;
        const int64_t tall = static_cast<int64_t>(outputHeight) * 16;

        int32_t extraX = 0;
        int32_t extraY = 0;
        if (wide > tall)
        {
            // Wider than 16:9: 0..1280 must land on a centered outputHeight*16/9 area, so the
            // logical range grows sideways by half the leftover width, in logical units.
            const int64_t safeWidth = static_cast<int64_t>(outputHeight) * 16 / 9;
            if (safeWidth <= 0) { return result; }
            extraX = static_cast<int32_t>((outputWidth - safeWidth) * 640 / safeWidth);
        }
        else if (wide < tall)
        {
            // Narrower - a 4:3 window, say: same correction rotated, the range grows
            // vertically and the interface sits vertically centered.
            const int64_t safeHeight = static_cast<int64_t>(outputWidth) * 9 / 16;
            if (safeHeight <= 0) { return result; }
            extraY = static_cast<int32_t>((outputHeight - safeHeight) * 360 / safeHeight);
        }
        else
        {
            return result;
        }

        const auto field = [object](uintptr_t offset) { return reinterpret_cast<int32_t*>(object + offset); };
        *field(kLayoutLogicalLeft) = -extraX;
        *field(kLayoutLogicalTop) = -extraY;
        *field(kLayoutLogicalRight) = 1280 + extraX;
        *field(kLayoutLogicalBottom) = 720 + extraY;

        static std::atomic<int> reported { 0 };
        if (reported.fetch_add(1) < 2)
        {
            spdlog::info("MGS4: UI Aspect: {}x{} output, logical range {}..{} x {}..{}.",
                outputWidth, outputHeight, -extraX, 1280 + extraX, -extraY, 720 + extraY);
        }

        return result;
    }

    // ======================= Layer 2: anchors to the corners =======================
    //
    // With the layout fix above, every widget is undistorted but the whole interface huddles
    // in a centered 16:9 area. This layer spreads the widgets back out: each one's NDC anchor
    // is scaled by outputWidth / safeWidth, which lands corner widgets at exactly the margins
    // they have on a 16:9 screen, keeps a centre anchor - the aiming reticle - where it
    // points, and leaves shapes alone because size lives in the basis vectors, not the anchor.
    //
    // The one subtlety is measured, not guessed (see docs/features/aspect-ratio.md): most
    // widgets share one anchor across all their quads, but some carry satellite anchors -
    // the ammo tick row is one anchor per tick, 0.008 NDC apart. Scaling each satellite
    // independently would grow the tick row past the box it sits in. So anchors are clustered
    // by draw order - consecutive records within a small epsilon belong to one widget - and
    // every record in a cluster gets the *same* translation, derived from the cluster's first
    // anchor. Within a widget everything keeps its exact relative layout.
    //
    // Applied at the game's generic upload helper (+7A8CD0: Map, memcpy, Unmap), on the
    // call that carries the per-frame instance pool. Finding this took three wrong turns,
    // all recorded in docs/features/aspect-ratio.md: the two "instance array" sites the
    // earlier research identified turn out to upload only tiny constant records, and the
    // pool's source pointer rides the stack as the helper's fifth argument, invisible to
    // any register-based probe. The pool itself is ~200KB, double-buffered, with 96-byte
    // anchor blocks at a clean pitch from a non-zero start offset.
    //
    // Blocks are recognised by signature, never by offset arithmetic: a real anchor block
    // has an axis-aligned basis - v6 = (2/W, 0), v7 = (0, -2/H) - and v8.z exactly 1.
    // Anything that fails the signature is left untouched, so a layout change in a future
    // game build degrades to a no-op rather than corruption.
    constexpr uintptr_t kRvaUploadHelper = 0x7A8CD0;
    constexpr uintptr_t kRvaInstancePoolCaller = 0x7A583F;    // return address of the pool upload call

    constexpr uint32_t kAnchorBlockV6 = 48;
    constexpr uint32_t kAnchorBlockV7 = 64;
    constexpr uint32_t kAnchorBlockV8 = 80;
    constexpr uint32_t kAnchorBlockSize = 96;

    // Satellites of one widget sit ~0.008 NDC apart, distinct widgets 0.15+, so anything
    // below this chains into the same cluster.
    constexpr float kClusterEpsilon = 0.05f;

    SafetyHookMid UploadHelper_hook {};

    bool PlausibleAnchorBlock(const uint8_t* block)
    {
        float v6[2] {};
        float v7[2] {};
        float v8z = 0.0f;
        std::memcpy(v6, block + kAnchorBlockV6, sizeof(v6));
        std::memcpy(v7, block + kAnchorBlockV7, sizeof(v7));
        std::memcpy(&v8z, block + kAnchorBlockV8 + 8, sizeof(v8z));
        return std::fabs(v6[1]) < 1e-6f && std::fabs(v7[0]) < 1e-6f
            && v6[0] > 0.0f && v6[0] < 1.0f && v7[1] < 0.0f && v7[1] > -1.0f
            && std::fabs(v8z - 1.0f) < 1e-4f;
    }

    // How much wider the output is than 16:9, or 1.0 when it is not. The same number the
    // layout fix compressed the anchors by, so scaling by it puts a widget at its 16:9
    // margins.
    float HorizontalSpreadFactor()
    {
        int32_t outputWidth = 0;
        int32_t outputHeight = 0;
        std::memcpy(&outputWidth, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(baseModule) + kRvaWindowSizeX), sizeof(outputWidth));
        std::memcpy(&outputHeight, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(baseModule) + kRvaWindowSizeY), sizeof(outputHeight));

        if (outputWidth < 320 || outputHeight < 240 || outputWidth > 16384 || outputHeight > 16384)
        {
            return 1.0f;
        }

        const int64_t safeWidth = static_cast<int64_t>(outputHeight) * 16 / 9;
        if (safeWidth <= 0 || safeWidth >= outputWidth)
        {
            return 1.0f;
        }
        return static_cast<float>(outputWidth) / static_cast<float>(safeWidth);
    }

    // Walk the pool: slide in 16-byte steps until a block matches, then advance at the
    // 96-byte pitch while blocks keep matching; a mismatch drops back to sliding and ends
    // the current cluster. Clusters translate as one piece - the delta comes from the
    // cluster's first anchor - so satellite anchors (the ammo ticks) keep their widget's
    // exact relative layout, and a centre anchor, the reticle, moves by zero.
    void SpreadPool(uint8_t* pool, size_t bytes)
    {
        const float factor = HorizontalSpreadFactor();
        if (factor == 1.0f || !pool || bytes < kAnchorBlockSize || bytes > (8u << 20))
        {
            return;
        }

        MEMORY_BASIC_INFORMATION info {};
        if (!VirtualQuery(pool, &info, sizeof(info)) || info.State != MEM_COMMIT
            || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            return;
        }
        const size_t usable = (std::min)(bytes,
            static_cast<size_t>(reinterpret_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize - pool));

        float previousX = 0.0f;
        float previousY = 0.0f;
        float delta = 0.0f;
        bool haveCluster = false;
        uint32_t touched = 0;

        size_t offset = 0;
        while (offset + kAnchorBlockSize <= usable)
        {
            if (!PlausibleAnchorBlock(pool + offset))
            {
                haveCluster = false;
                offset += 16;
                continue;
            }

            float v8[2] {};
            std::memcpy(v8, pool + offset + kAnchorBlockV8, sizeof(v8));

            // Only canvas-authored anchors are spread. Under the layout fix the 1280x720
            // canvas occupies |x| <= 1/factor of NDC, so every widget anchor lands inside
            // that; an anchor beyond it was authored against the full output through the
            // expanded logical range - the fullscreen fades and vignettes pinned at exactly
            // (+-1, +-1). Spreading one of those slides its coverage sideways and uncovers a
            // strip of the frame: measured as an orange band over the right sixth of the
            // screen, exactly the (factor-1)/2 fraction the arithmetic predicts.
            if (std::fabs(v8[0]) > 1.0f / factor + 0.001f)
            {
                haveCluster = false;
                offset += kAnchorBlockSize;
                continue;
            }

            if (!haveCluster
                || std::fabs(v8[0] - previousX) > kClusterEpsilon
                || std::fabs(v8[1] - previousY) > kClusterEpsilon)
            {
                delta = v8[0] * (factor - 1.0f);
                haveCluster = true;
            }
            previousX = v8[0];
            previousY = v8[1];

            const float spread = v8[0] + delta;
            std::memcpy(pool + offset + kAnchorBlockV8, &spread, sizeof(spread));
            touched++;
            offset += kAnchorBlockSize;
        }

        static std::atomic<int> reported { 0 };
        if (touched > 0 && reported.fetch_add(1) < 3)
        {
            spdlog::info("MGS4: UI Aspect: spread {} anchors (factor {:.4f}).", touched, factor);
        }
    }

    void UploadHelperProbe(SafetyHookContext& ctx)
    {
        // Only the instance pool upload; every other upload through this helper - vertex
        // data, textures, the constant records - passes untouched at the cost of one
        // compare.
        uintptr_t returnAddress = 0;
        std::memcpy(&returnAddress, reinterpret_cast<const void*>(ctx.rsp), sizeof(returnAddress));
        if (returnAddress != reinterpret_cast<uintptr_t>(baseModule) + kRvaInstancePoolCaller)
        {
            return;
        }

        // The helper takes (wrapper, resource, destOffset, size) in registers; the source
        // is the fifth argument, on the stack.
        uintptr_t source = 0;
        std::memcpy(&source, reinterpret_cast<const void*>(ctx.rsp + 0x28), sizeof(source));
        SpreadPool(reinterpret_cast<uint8_t*>(source), ctx.r9);
    }

    void ApplyCornerSpread()
    {
        if (!bFixUiAspectCorners)
        {
            return;
        }

        UploadHelper_hook = safetyhook::create_mid(
            reinterpret_cast<uint8_t*>(baseModule) + kRvaUploadHelper, UploadHelperProbe);
        LOG_HOOK(UploadHelper_hook, "MGS4: UI Aspect corners")
    }

    void ApplyFixes()
    {
        if (!bFixUiAspect)
        {
            return;
        }

        auto* const at = reinterpret_cast<uint8_t*>(baseModule) + kRvaUiLayoutConverter;

        // The RVA is specific to one executable build and .text is encrypted on disk, so the
        // only meaningful check is here, in-process, before hooking. MSVC's canonical
        // home-register prologue; anything else means a different binary.
        constexpr uint8_t kExpectedPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
        if (std::memcmp(at, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0)
        {
            spdlog::error("MGS4: UI Aspect: +{:X} does not start with the expected prologue - "
                "wrong executable build, not hooking.", kRvaUiLayoutConverter);
            return;
        }

        UiLayoutConverter_hook = safetyhook::create_inline(at, reinterpret_cast<void*>(&ConvertLayout));
        LOG_HOOK(UiLayoutConverter_hook, "MGS4: UI Aspect")

        // Layer 2 rides on layer 1: without the undistorted centered base, spreading anchors
        // would just rearrange stretched widgets.
        if (UiLayoutConverter_hook)
        {
            ApplyCornerSpread();
        }
    }
}
