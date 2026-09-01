#include "stdafx.h"

#include "aspect_ratio.hpp"

#include "common.hpp"
#include "logging.hpp"

#include <mutex>

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

    // Which anchors to shift, as zones in 16:9 NDC. A dead zone plus "shift everything
    // outside it" had a fatal flaw the user caught in play: world-tracking elements - the
    // pickup label hovering over a dropped weapon, enemy markers - share the anchor space
    // with static widgets, and the moment one panned across the threshold it jumped 480px
    // off its target. No position rule separates "widget" from "thing tracking the world",
    // because the world can put an element anywhere.
    //
    // So the logic is inverted: only anchors inside these measured widget zones shift, and
    // everything unknown stays centered. That fails safe in both directions - a world-tracked
    // element never jumps, and a widget missing from this table sits at the centered position
    // rather than anywhere wrong, which is cosmetic, easy to spot in play, and a one-line
    // addition here. Zones come from the anchor census (16:9 values, generous margins);
    // at runtime anchors are normalized by the compression factor before the lookup, so one
    // table serves every output aspect.
    struct WidgetZone
    {
        float minX, maxX, minY, maxY;
    };
    constexpr WidgetZone kWidgetZones[] = {
        { -0.96f, -0.74f,  0.76f, 0.96f },   // life bar / stress cluster, top-left (-0.881, 0.883)
        { -0.82f, -0.60f, -0.86f, -0.66f },  // ration box, bottom-left (-0.719, -0.769)
        {  0.46f,  0.82f, -0.86f, -0.61f },  // weapon box, ammo ticks, item box, bottom-right
        {  0.61f,  0.84f,  0.50f, 0.74f },   // camo meter / compass, top-right (0.733, 0.622)
    };

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

    // Rotated quads - the compass's facing arrow, the diagonal weapon silhouette, the
    // objective marker - carry a rotated basis, which the strict test above rejects by
    // design. Left unspread they stay at their centered positions while their widgets move,
    // which is exactly the defect the user reported. This relaxed form accepts any rotation
    // but is only ever consulted *inside* an established 96-byte run, where the structural
    // context does the work the axis test cannot: a random float block passing v8.z == 1 and
    // four sane basis magnitudes AND sitting at exact pitch inside a verified anchor run is
    // not something mesh data produces.
    bool RelaxedAnchorBlock(const uint8_t* block)
    {
        float v6[2] {};
        float v7[2] {};
        float v8z = 0.0f;
        std::memcpy(v6, block + kAnchorBlockV6, sizeof(v6));
        std::memcpy(v7, block + kAnchorBlockV7, sizeof(v7));
        std::memcpy(&v8z, block + kAnchorBlockV8 + 8, sizeof(v8z));
        const float magnitude = std::fabs(v6[0]) + std::fabs(v6[1])
            + std::fabs(v7[0]) + std::fabs(v7[1]);
        return std::fabs(v8z - 1.0f) < 1e-4f
            && magnitude > 0.0f && magnitude < 2.0f
            && std::fabs(v6[0]) < 1.0f && std::fabs(v6[1]) < 1.0f
            && std::fabs(v7[0]) < 1.0f && std::fabs(v7[1]) < 1.0f;
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
    // 96-byte pitch while blocks keep matching; a mismatch drops back to sliding.
    //
    // The shift itself is a constant per side with a dead zone in the middle - deliberately
    // NOT a function of each anchor's exact position. The first version scaled every anchor
    // by the spread factor, grouped by a draw-order proximity heuristic, and the user's
    // comparison shots showed what any position-dependent delta does inside a widget: the
    // reticle's corner dots spread into a rectangle, the weapon silhouette slid out of its
    // box, the compass lost its bullseye and its N drifted. A widget only survives if every
    // one of its parts moves by exactly the same amount.
    //
    // The constant is 1 - 1/factor per side, and that number is not arbitrary: compressed
    // NDC times full output width equals safe-canvas NDC times safe width, so this shift
    // preserves each widget's *pixel* distance from the screen edge exactly as it is on a
    // 16:9 output. Corners land at their 16:9 margins; everything inside a widget stays
    // pixel-rigid; the reticle and its dots sit in the dead zone and never move.
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

        const float canvasEdge = 1.0f / factor;
        const float shift = 1.0f - canvasEdge;
        uint32_t touched = 0;

        size_t offset = 0;
        bool inRun = false;
        while (offset + kAnchorBlockSize <= usable)
        {
            const bool strict = PlausibleAnchorBlock(pool + offset);
            if (!strict && !(inRun && RelaxedAnchorBlock(pool + offset)))
            {
                inRun = false;
                offset += 16;
                continue;
            }
            inRun = true;

            float v8[2] {};
            std::memcpy(v8, pool + offset + kAnchorBlockV8, sizeof(v8));

            // Normalize x back to 16:9 NDC (the compression only touches x). Three regimes:
            //
            // - Whitelisted widgets: constant shift, preserving 16:9 pixel margins and rigid
            //   multi-anchor layout (ammo ticks, crosshair dots).
            // - Beyond the canvas edge: output-authored fullscreen layers, untouched.
            // - Everything else - the Solid Eye's world-tracked labels above all: the game
            //   projects them through the full-width camera and converts through fixed
            //   1280-canvas math, so the widened canvas compresses them toward centre by
            //   1/factor and they drift off their targets. The correction is the exact
            //   inverse: scale the anchor back out, which moves the label without stretching
            //   it (shape lives in the basis vectors). A small centre dead zone keeps the
            //   reticle's separate corner-dot anchors square, and a ramp between the two
            //   keeps the transition continuous so nothing pops while the camera pans.
            const float normX = v8[0] * factor;
            if (std::fabs(normX) > 1.001f)
            {
                offset += kAnchorBlockSize;
                continue;
            }

            bool isWidget = false;
            for (const auto& zone : kWidgetZones)
            {
                if (normX >= zone.minX && normX <= zone.maxX
                    && v8[1] >= zone.minY && v8[1] <= zone.maxY)
                {
                    isWidget = true;
                    break;
                }
            }

            float spread = v8[0];
            if (isWidget)
            {
                spread = v8[0] + (v8[0] > 0.0f ? shift : -shift);
            }
            else
            {
                constexpr float kDead = 0.06f;   // reticle dots live at |normX| ~0.03
                constexpr float kRamp = 0.30f;   // fully world-accurate from here outward
                const float n = std::fabs(normX);
                if (n > kDead)
                {
                    const float target = (n >= kRamp)
                        ? n
                        : kDead / factor + (n - kDead) * (kRamp - kDead / factor) / (kRamp - kDead);
                    spread = (normX > 0.0f) ? target : -target;
                }
            }

            if (spread != v8[0])
            {
                std::memcpy(pool + offset + kAnchorBlockV8, &spread, sizeof(spread));
                touched++;
            }
            offset += kAnchorBlockSize;
        }

        static std::atomic<int> reported { 0 };
        if (touched > 0 && reported.fetch_add(1) < 3)
        {
            spdlog::info("MGS4: UI Aspect: shifted {} widget anchors by {:.4f} per side.",
                touched, 1.0f - 1.0f / factor);
        }
    }

    // Temporary census of every anchor-bearing pool that flows through the helper. Three HUD
    // elements sit at centered positions while their widgets spread - the compass's centre
    // arrow, the weapon icon, the objective marker - which means they upload through calls
    // the +7A583F gate does not cover. This logs each distinct source buffer once, with its
    // caller, hit count and sample anchors, so the spread policy can be decided per pool
    // rather than guessed: a world-tracking marker may need to stay unspread on purpose.
    struct PoolVerdict
    {
        uintptr_t pointer;
        bool spread;
    };
    std::mutex g_PoolCacheMutex;
    std::vector<PoolVerdict> g_PoolCache;

    uint32_t ScanForAnchors(const uint8_t* buffer, size_t bytes, size_t* firstOffset, float* sampleV8)
    {
        uint32_t hits = 0;
        const size_t scanEnd = (std::min<size_t>)(bytes, 64u << 10);
        if (scanEnd < kAnchorBlockSize)
        {
            return 0;
        }
        for (size_t offset = 0; offset + kAnchorBlockSize <= scanEnd; offset += 16)
        {
            if (PlausibleAnchorBlock(buffer + offset))
            {
                if (hits == 0)
                {
                    *firstOffset = offset;
                    std::memcpy(sampleV8, buffer + offset + kAnchorBlockV8, 2 * sizeof(float));
                }
                hits++;
            }
        }
        return hits;
    }

    void UploadHelperProbe(SafetyHookContext& ctx)
    {
        uintptr_t returnAddress = 0;
        std::memcpy(&returnAddress, reinterpret_cast<const void*>(ctx.rsp), sizeof(returnAddress));
        const auto base = reinterpret_cast<uintptr_t>(baseModule);
        const uintptr_t retRva = (returnAddress >= base) ? returnAddress - base : returnAddress;

        // The helper takes (wrapper, resource, destOffset, size) in registers; the source
        // is the fifth argument, on the stack.
        uintptr_t source = 0;
        std::memcpy(&source, reinterpret_cast<const void*>(ctx.rsp + 0x28), sizeof(source));
        if (!source || ctx.r9 < kAnchorBlockSize || ctx.r9 > (8u << 20))
        {
            return;
        }

        // One verdict per distinct source buffer; the pools are stable ping-pong pairs, so
        // the cache stays tiny and every later call skips the scan.
        bool known = false;
        bool spread = false;
        {
            std::lock_guard lock(g_PoolCacheMutex);
            for (const auto& entry : g_PoolCache)
            {
                if (entry.pointer == source)
                {
                    known = true;
                    spread = entry.spread;
                    break;
                }
            }
        }

        if (!known)
        {
            MEMORY_BASIC_INFORMATION info {};
            if (!VirtualQuery(reinterpret_cast<void*>(source), &info, sizeof(info))
                || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            {
                return;
            }

            size_t firstOffset = 0;
            float sampleV8[2] {};
            const uint32_t hits = ScanForAnchors(reinterpret_cast<const uint8_t*>(source),
                ctx.r9, &firstOffset, sampleV8);

            // A buffer with no anchors right now is not a verdict - the pools appear at boot
            // before any UI is in them, and caching that first empty look disabled the spread
            // for the whole session. Only a buffer that has shown anchors gets cached.
            if (hits < 3)
            {
                return;
            }

            // Policy for now: spread only the pool the working milestone used; log the rest.
            spread = (retRva == kRvaInstancePoolCaller);

            spdlog::info("MGS4: UI Aspect: pool {:X} size={} ret=+{:X} hits={} first=+{} v8=({:.4f},{:.4f}){}",
                source, ctx.r9, retRva, hits, firstOffset, sampleV8[0], sampleV8[1],
                spread ? "" : "   <== NOT SPREAD");

            std::lock_guard lock(g_PoolCacheMutex);
            if (g_PoolCache.size() < 64)
            {
                g_PoolCache.push_back({ source, spread });
            }
        }

        if (spread)
        {
            SpreadPool(reinterpret_cast<uint8_t*>(source), ctx.r9);
        }
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
