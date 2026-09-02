#include "stdafx.h"

#include "aspect_ratio.hpp"

#include "common.hpp"
#include "logging.hpp"
#include "resolution_scaling_fixes.hpp"

#include <algorithm>
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

    // Every layout object the converter touches, in creation order - the live lab's address
    // book. Objects are engine-owned heap allocations; entries can go stale on stage
    // transitions, so every live write re-validates readability first.
    struct KnownObject
    {
        uintptr_t object;
        uint32_t strcode;
        bool pairedAfterRationPanel;   // a 0059FFD5 created right after a 0029FC73
    };
    std::mutex g_ObjectsMutex;
    std::vector<KnownObject> g_Objects;
    int32_t g_LiveExtraX = 0;   // layer-1 extraX, for reset commands

    bool KnownObjectStillValid(const KnownObject& known);

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

        {
            uint32_t strcodeForRegistry = 0;
            std::memcpy(&strcodeForRegistry, reinterpret_cast<const void*>(object + 0x1C), sizeof(strcodeForRegistry));
            std::lock_guard lock(g_ObjectsMutex);
            const bool paired = strcodeForRegistry == 0x0059FFD5
                && !g_Objects.empty() && g_Objects.back().strcode == 0x0029FC73;
            g_Objects.push_back({ object, strcodeForRegistry, paired });
            g_LiveExtraX = extraX;
        }

        const auto field = [object](uintptr_t offset) { return reinterpret_cast<int32_t*>(object + offset); };
        *field(kLayoutLogicalLeft) = -extraX;
        *field(kLayoutLogicalTop) = -extraY;
        *field(kLayoutLogicalRight) = 1280 + extraX;
        *field(kLayoutLogicalBottom) = 720 + extraY;

        // Layer 2, in its final form: per-widget corner anchoring through the object's own
        // mapping. Each layout object carries its la2 file's strcode at +0x1C, each HUD
        // widget turned out to live in its own la2 (measured with the ladder experiment -
        // every widget shifted by its own object's bias), and translating the logical range
        // moves the whole widget rigidly: no quad ever inspected, nothing unintended ever
        // touched, world-tracked elements in other objects untouched by construction.
        //
        // align -1 pins the object's canvas to the left screen edge, +1 to the right,
        // absent means centered. The table is measured, not guessed - see the feature doc.
        if (bFixUiAspectCorners && extraX > 0)
        {
            // firstInstance entries anchor only the first live object of the strcode: the
            // bottom-corner widgets' la2 files are instantiated many times - one gameplay
            // HUD copy plus a copy per weapon/item-selector slot and menu screen (measured
            // live: ten 0029FC73/0059FFD5 pairs at once, and per-instance bias probes moved
            // the visible panels only from the first pair) - and the gameplay copy is always
            // created first. Anchoring every copy dragged the selectors to the corners
            // (AR-005). "Live" matters for stage reloads: dead earlier instances must not
            // veto the new HUD copy, so earlier instances are identity-checked, not counted.
            struct WidgetAnchor { uint32_t strcode; int align; bool firstInstance; };
            static constexpr WidgetAnchor kWidgetAnchors[] = {
                { 0x00B66172, -1, false },   // life bar / stress, top-left ("OLD SNAKE" in its la2)
                { 0x003E6492, +1, false },   // camo meter / compass, top-right ("CAMO %")
                { 0x0029FC73, -1, true },    // ration panel, bottom-left backing plate
                { 0x002506A9, -1, true },    // ration content: icon, count, name
                { 0x0059FFD5, +1, true },    // weapon panel, bottom-right backing plate
                { 0x00CD1BC3, +1, true },    // weapon content: silhouette, ammo, name
            };

            uint32_t strcode = 0;
            std::memcpy(&strcode, reinterpret_cast<const void*>(object + 0x1C), sizeof(strcode));

            for (const auto& anchor : kWidgetAnchors)
            {
                if (anchor.strcode != strcode)
                {
                    continue;
                }

                if (anchor.firstInstance)
                {
                    bool earlierLiveInstance = false;
                    {
                        std::lock_guard lock(g_ObjectsMutex);
                        for (const auto& known : g_Objects)
                        {
                            if (known.object != object && known.strcode == strcode
                                && KnownObjectStillValid(known))
                            {
                                earlierLiveInstance = true;
                                break;
                            }
                        }
                    }
                    if (earlierLiveInstance)
                    {
                        spdlog::info("MGS4: UI Aspect: la2 {:08X} later instance left centered.", strcode);
                        break;
                    }
                }

                // Same scale, shifted window: left-pinned means logical 0 lands on screen
                // x 0, right-pinned means logical 1280 lands on the right edge.
                *field(kLayoutLogicalLeft) = -extraX * (1 + anchor.align);
                *field(kLayoutLogicalRight) = 1280 + extraX * (1 - anchor.align);
                spdlog::info("MGS4: UI Aspect: la2 {:08X} anchored {}.",
                    strcode, anchor.align < 0 ? "left" : "right");
                break;
            }
        }

        // Identity experiments: each layout object carries its la2 file's strcode at +0x1C
        // (measured; independently corroborated by mgs4Ultra120 naming 0x3D7604 the title
        // menu resource).
        //
        // Single mode: bias the one matching object hard and see what moves.
        if (uAspectTestStrcode != 0)
        {
            uint32_t strcode = 0;
            std::memcpy(&strcode, reinterpret_cast<const void*>(object + 0x1C), sizeof(strcode));
            if (strcode == uAspectTestStrcode)
            {
                // Vertical occurrence ladder: the Nth object of the file rises by N steps, so
                // one run shows which occurrence hosts the visible widget - built for
                // 0059FFD5, whose 562 objects hold the corner weapon box, the selectors and
                // the bullseye-sensitive pieces all in one file.
                static std::atomic<int> testOccurrence { 0 };
                const int occurrence = testOccurrence.fetch_add(1);
                *field(kLayoutLogicalTop) += occurrence * 30;
                *field(kLayoutLogicalBottom) += occurrence * 30;
                spdlog::info("MGS4: UI Aspect: test bias occurrence {} on object {:X} (la2 {:08X}).",
                    occurrence, object, strcode);
            }
        }

        // Ladder mode: every distinct strcode gets its own bias, index x 12 logical units, so
        // one run maps every visible widget to its host object - measure a widget's pixel
        // shift against an unbiased baseline, divide by the per-step shift, and the index
        // names the strcode from this log. Built after two single-file guesses both missed:
        // only 31 of 176 loaded la2 files get layout objects at all, and the gameplay HUD's
        // own file is not among them, so it renders through some host object here.
        if (bAspectTestLadder)
        {
            uint32_t strcode = 0;
            std::memcpy(&strcode, reinterpret_cast<const void*>(object + 0x1C), sizeof(strcode));

            static std::mutex ladderMutex;
            static std::vector<uint32_t> ladder;
            int index = -1;
            {
                std::lock_guard lock(ladderMutex);
                for (size_t i = 0; i < ladder.size(); i++)
                {
                    if (ladder[i] == strcode) { index = static_cast<int>(i); break; }
                }
                if (index < 0 && ladder.size() < 64)
                {
                    ladder.push_back(strcode);
                    index = static_cast<int>(ladder.size()) - 1;
                }
            }
            if (index >= 0)
            {
                *field(kLayoutLogicalLeft) -= index * 24;
                *field(kLayoutLogicalRight) -= index * 24;
                spdlog::info("MGS4: UI Aspect ladder: object {:X} la2 {:08X} index {} bias {}.",
                    object, strcode, index, index * 24);
            }
        }

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
    // The selector-wheel signature, measured with pooldump on a live save (2026-09-01,
    // AR-008): the wheel is a cross of slot anchors rooted at the box position - a column
    // at exactly the box x (|normX| = 0.719) climbing above the box band in slot steps,
    // and a row at exactly the box y (-0.769) stepping toward centre at pitch 0.3595.
    // The box's own cluster (ammo ticks at y=-0.7785, parts at -0.67) appears only while
    // the wheel is closed; the cross only while it is held open. So the pool itself says
    // whether a wheel is open, per frame, with no engine flag: three or more column
    // anchors above the box band is a wheel.
    constexpr float kWheelAxisX = 0.719f;
    constexpr float kWheelRowY = -0.769f;

    struct PoolState
    {
        bool weaponOpen;   // right-side wheel (column at +0.719)
        bool itemOpen;     // left-side wheel (column at -0.719)
        bool menuOpen;     // fullscreen menu (rails and sidebars carpeting the near-edge band)
    };

    PoolState DetectPoolState(const uint8_t* pool, size_t usable, float factor)
    {
        int right = 0;
        int left = 0;
        int nearEdge = 0;
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
            const float normX = v8[0] * factor;
            if (v8[1] > -0.58f && v8[1] < 0.35f)
            {
                if (std::fabs(normX - kWheelAxisX) < 0.02f) { right++; }
                if (std::fabs(normX + kWheelAxisX) < 0.02f) { left++; }
            }
            // The near-edge band: normalized past every gameplay widget but short of the
            // raw +-1 fullscreen layers. Gameplay produces the odd world label here; the
            // pause menu's frame rails, side text and borders carpet it.
            if (std::fabs(v8[0]) < 0.999f && std::fabs(normX) > 0.90f)
            {
                nearEdge++;
            }
            offset += kAnchorBlockSize;
        }
        return { right >= 3, left >= 3, nearEdge >= 4 };
    }

    // Whether an anchor belongs to the open wheel's cross (side +1 = weapon wheel bottom-
    // right, -1 = item wheel bottom-left), in normalized 16:9 coordinates. Deliberately
    // generous, and measured the hard way: with a full inventory the row runs from the
    // junction *past centre to the far side* (a five-item row reaches x = +0.72 on the
    // item wheel), so while a wheel is open the whole bottom band is the wheel's - safe
    // because the game hides the opposite corner box while either selector is up
    // (ps_itm.png). The column likewise climbs to +0.8 with a tall inventory, and slot
    // adornments (SUP/light icons, CQC tags, mode labels) hang off it. Wheel-open frames
    // are transient, so a world element passing through these bands is displaced only
    // while the player is actively holding the selector.
    bool WheelMember(float normX, float y, int side)
    {
        const float x = normX * side;
        const bool row = y > -0.87f && y < -0.58f && x > -0.88f && x < 0.88f;
        const bool column = y >= -0.58f && y < 0.88f && x > 0.48f && x < 0.88f;
        return row || column;
    }

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

        const PoolState wheels = DetectPoolState(pool, usable, factor);

        // A fullscreen menu is screen-space UI through and through - the user's stated
        // preference is centered ("letterboxed") menus - and the game draws no world
        // labels beneath it, so the right treatment for a menu frame is no treatment:
        // the layer-1 centered canvas, untouched. Without this the world-decompression
        // ramp spreads the menu's own panels back out to full width and tears its frame
        // (ps2_menu.png).
        if (wheels.menuOpen)
        {
            return;
        }

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
            // The fullscreen exemption keys on the *compressed* coordinate, not the
            // normalized one: output-authored layers are pinned at exactly +-1 as written,
            // while a world-tracked label part near the screen edge sits at a compressed
            // |x| around 0.75..0.9 - normalized past 1.0. An earlier version skipped
            // anything with |normX| > 1, and a label straddling that line had some parts
            // decompressed outward and some left inboard: its text visibly swapped sides
            // near the screen edge.
            const float normX = v8[0] * factor;
            if (std::fabs(v8[0]) > 0.999f)
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

            // An open wheel's whole cross moves with its box, so the wheel arrives at the
            // corner rigid - the junction slot IS the box, and 16:9 composes the cross
            // around the corner. The membership test subsumes the box zone on that side.
            int wheelSide = 0;
            if (wheels.weaponOpen && WheelMember(normX, v8[1], +1)) { wheelSide = +1; }
            else if (wheels.itemOpen && WheelMember(normX, v8[1], -1)) { wheelSide = -1; }

            float spread = v8[0];
            if (wheelSide != 0)
            {
                spread = v8[0] + wheelSide * shift;
            }
            else if (isWidget)
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

    // One-shot dump of every anchor block in the live instance pool, armed by the live
    // command "pooldump". The AR-008 state experiment: dump with the wheel closed, dump
    // with it held open, and the box's anchor either moves out of the corner zone (the
    // zone-gated draw-time spread works) or it does not (it cannot).
    std::atomic<int> g_PoolDumpBudget { 0 };

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

    // Finds the two reference anchors in the live pool and arms a write watch on each: the
    // ration box (static widget, anchor a fixed layout constant) and the aiming reticle
    // (dynamic, computed per frame from the aim - the element the truncation bug wrapped).
    // Values are matched in normalized 16:9 NDC so the same targets work at any aspect;
    // run with the corners spread off so the pool holds the game's own numbers.
    void WatchProducersInPool(uint8_t* pool, size_t bytes)
    {
        static std::atomic<bool> staticArmed { false };
        static std::atomic<bool> dynamicArmed { false };
        if (staticArmed.load() && dynamicArmed.load())
        {
            return;
        }

        const float factor = HorizontalSpreadFactor();

        MEMORY_BASIC_INFORMATION info {};
        if (!VirtualQuery(pool, &info, sizeof(info)) || info.State != MEM_COMMIT
            || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            return;
        }
        const size_t usable = (std::min)(bytes,
            static_cast<size_t>(reinterpret_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize - pool));

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
            const float normX = v8[0] * factor;
            const uintptr_t slotAddress = reinterpret_cast<uintptr_t>(pool + offset + kAnchorBlockV8);

            bool expected = false;
            if (std::fabs(normX + 0.719f) < 0.04f && std::fabs(v8[1] + 0.769f) < 0.04f
                && staticArmed.compare_exchange_strong(expected, true))
            {
                ResolutionScalingFixes::ArmProducerWatch(slotAddress,
                    "the static producer (ration box anchor)");
            }

            expected = false;
            if (std::fabs(normX) < 0.04f && v8[1] > 0.10f && v8[1] < 0.40f
                && dynamicArmed.compare_exchange_strong(expected, true))
            {
                ResolutionScalingFixes::ArmProducerWatch(slotAddress,
                    "the dynamic producer (reticle anchor)");
            }

            offset += kAnchorBlockSize;
        }
    }

    void UploadHelperProbe(SafetyHookContext& ctx)
    {
        // The helper is one of the game's hottest paths (every buffer upload). The hook
        // now installs unconditionally so the lab can arm diagnostics mid-session, so the
        // probe must cost one flag check when nothing is armed.
        if (!bAspectPoolSpread && !ResolutionScalingFixes::bWatchAnchorProducers
            && !bAspectTestLadder && g_PoolDumpBudget.load(std::memory_order_relaxed) <= 0)
        {
            return;
        }

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

        // Draw-time corner anchoring (AR-008 experiment): the spread applied to the live
        // instance pool, wheel-state read from the pool itself inside SpreadPool.
        if (bAspectPoolSpread && spread)
        {
            SpreadPool(reinterpret_cast<uint8_t*>(source), ctx.r9);
        }

        // Producer identification mode: locate the two reference blocks in the live pool and
        // put hardware write watches on their v8.x slots, so the code that computes each kind
        // of anchor names itself.
        if (ResolutionScalingFixes::bWatchAnchorProducers && spread)
        {
            WatchProducersInPool(reinterpret_cast<uint8_t*>(source), ctx.r9);
        }

        // Ladder pool report: with the object ladder active, read the answer from the pool
        // instead of the screen. The user identified why every screen-side sweep failed -
        // the loaded asset set is dominated by latent UI that never renders - so this starts
        // from the *rendered* values: log every anchor block in the bottom-widget y band,
        // and offline arithmetic (x = rest + 0.02813 * index) names the host object's ladder
        // index exactly, visibility and occlusion irrelevant.
        if (bAspectTestLadder && spread)
        {
            static std::atomic<int> reportBudget { 400 };
            uint8_t* pool = reinterpret_cast<uint8_t*>(source);
            const size_t bytes = (std::min<uint64_t>)(ctx.r9, 1u << 20);
            size_t offset = 0;
            bool inRun = false;
            while (offset + kAnchorBlockSize <= bytes)
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
                if (v8[1] > -0.85f && v8[1] < -0.60f && reportBudget.fetch_sub(1) > 0)
                {
                    spdlog::info("MGS4: ladder pool: v8=({:.5f},{:.5f})", v8[0], v8[1]);
                }
                offset += kAnchorBlockSize;
            }
        }

        // One-shot pool dump: every anchor block's v8, raw and normalized, in draw order.
        // Only the instance pool (the caller the working milestone spread) is dumped, and
        // the budget is consumed by the dump so one command yields one frame's snapshot.
        if (spread && g_PoolDumpBudget.load() > 0
            && g_PoolDumpBudget.exchange(0) > 0)
        {
            const float factor = HorizontalSpreadFactor();
            uint8_t* pool = reinterpret_cast<uint8_t*>(source);
            const size_t bytes = (std::min<uint64_t>)(ctx.r9, 1u << 20);
            spdlog::info("MGS4: pooldump: begin pool={:X} size={} factor={:.4f}", source, ctx.r9, factor);
            int logged = 0;
            size_t offset = 0;
            bool inRun = false;
            while (offset + kAnchorBlockSize <= bytes && logged < 600)
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
                spdlog::info("MGS4: pooldump: +{:05X} v8=({:.5f},{:.5f}) norm=({:.5f},{:.5f}){}",
                    offset, v8[0], v8[1], v8[0] * factor, v8[1],
                    strict ? "" : " r");
                logged++;
                offset += kAnchorBlockSize;
            }
            spdlog::info("MGS4: pooldump: end, {} blocks.", logged);
        }

        // The pool spread that used to live here is gone: corner anchoring now happens
        // per-object in the layout converter, keyed on each object's la2 strcode. This hook
        // remains solely as the mount point for the diagnostics above.
    }

    // ---- producer attribution probe --------------------------------------------------------
    //
    // The identity experiment the per-object plan needs (methodology set by the user,
    // 2026-09-01): at the moment an anchor is composed into the pool, some frame in the
    // producer call chain must hold the layout object being evaluated - and a layout
    // object self-identifies (strcode at +0x1C, the two engine pointers at +0x20/+0x28).
    // The write-watch mapped the chain precisely: memcpy <- +68A938 <- +666485 <- +666729
    // <- +BAFDF <- +BE330, with +666470 a thin wrapper entry and +BAFC0 a function entry.
    // This probe sits on those two entries and, while armed (live command "producer"),
    // scans every GP register and the top of the stack for pointers that validate as
    // layout objects, logging (site, location, object, strcode). If the object is
    // recoverable here, per-object positioning replaces every positional heuristic.
    constexpr uintptr_t kRvaProducerWrapper = 0x666470;
    constexpr uintptr_t kRvaProducerOuter = 0xBAFC0;
    SafetyHookMid ProducerWrapper_hook {};
    SafetyHookMid ProducerOuter_hook {};
    std::atomic<int> g_ProducerProbeBudget { 0 };

    // Whether an arbitrary pointer is a live layout object. The engine-pointer pair is
    // the discriminator; the strcode is payload. Reads are guarded the same way the
    // registry validation is.
    bool LooksLikeLayoutObject(uintptr_t candidate, uint32_t* strcodeOut)
    {
        if (candidate < 0x10000 || (candidate & 0x7))
        {
            return false;
        }
        MEMORY_BASIC_INFORMATION info {};
        if (!VirtualQuery(reinterpret_cast<void*>(candidate), &info, sizeof(info))
            || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            || !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
        {
            return false;
        }
        const auto regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (candidate + 0x30 > regionEnd)
        {
            return false;
        }
        uintptr_t engineA = 0;
        uintptr_t engineB = 0;
        std::memcpy(&engineA, reinterpret_cast<const void*>(candidate + 0x20), sizeof(engineA));
        std::memcpy(&engineB, reinterpret_cast<const void*>(candidate + 0x28), sizeof(engineB));
        const auto moduleBase = reinterpret_cast<uintptr_t>(baseModule);
        if (engineA != moduleBase + 0x437150 || engineB != moduleBase + 0x437110)
        {
            return false;
        }
        std::memcpy(strcodeOut, reinterpret_cast<const void*>(candidate + 0x1C), sizeof(*strcodeOut));
        return true;
    }

    void ProbeProducerSite(const char* site, const SafetyHookContext& ctx)
    {
        if (g_ProducerProbeBudget.fetch_sub(1) <= 0)
        {
            g_ProducerProbeBudget.store(0);
            return;
        }

        struct Candidate { const char* name; uintptr_t value; };
        const Candidate registers[] = {
            { "rcx", ctx.rcx }, { "rdx", ctx.rdx }, { "r8", ctx.r8 }, { "r9", ctx.r9 },
            { "rbx", ctx.rbx }, { "rbp", ctx.rbp }, { "rsi", ctx.rsi }, { "rdi", ctx.rdi },
            { "r10", ctx.r10 }, { "r11", ctx.r11 }, { "r12", ctx.r12 }, { "r13", ctx.r13 },
            { "r14", ctx.r14 }, { "r15", ctx.r15 },
        };

        bool found = false;
        for (const auto& reg : registers)
        {
            uint32_t strcode = 0;
            if (LooksLikeLayoutObject(reg.value, &strcode))
            {
                spdlog::info("MGS4: producer probe [{}]: {} = obj {:X} la2 {:08X} (args rcx={:X} rdx={:X})",
                    site, reg.name, reg.value, strcode, ctx.rcx, ctx.rdx);
                found = true;
            }
        }

        // The top of the caller's stack: spill slots and stack arguments. 16 qwords is
        // deliberately shallow - a deep walk finds stale objects from earlier frames and
        // muddies the attribution.
        for (int slot = 0; slot < 16; slot++)
        {
            uintptr_t value = 0;
            std::memcpy(&value, reinterpret_cast<const void*>(ctx.rsp + slot * 8), sizeof(value));
            uint32_t strcode = 0;
            if (LooksLikeLayoutObject(value, &strcode))
            {
                spdlog::info("MGS4: producer probe [{}]: rsp+{:X} = obj {:X} la2 {:08X}",
                    site, slot * 8, value, strcode);
                found = true;
            }
        }

        if (!found)
        {
            spdlog::info("MGS4: producer probe [{}]: no object in frame (rcx={:X} rdx={:X} r8={:X} r9={:X})",
                site, ctx.rcx, ctx.rdx, ctx.r8, ctx.r9);
        }
    }

    void ProducerWrapperProbe(SafetyHookContext& ctx)
    {
        if (g_ProducerProbeBudget.load(std::memory_order_relaxed) <= 0) { return; }
        ProbeProducerSite("wrapper+666470", ctx);
    }

    void ProducerOuterProbe(SafetyHookContext& ctx)
    {
        if (g_ProducerProbeBudget.load(std::memory_order_relaxed) <= 0) { return; }
        ProbeProducerSite("outer+BAFC0", ctx);
    }

    void InstallProducerProbe()
    {
        ProducerWrapper_hook = safetyhook::create_mid(
            reinterpret_cast<uint8_t*>(baseModule) + kRvaProducerWrapper, ProducerWrapperProbe);
        LOG_HOOK(ProducerWrapper_hook, "MGS4: producer probe (wrapper)")
        ProducerOuter_hook = safetyhook::create_mid(
            reinterpret_cast<uint8_t*>(baseModule) + kRvaProducerOuter, ProducerOuterProbe);
        LOG_HOOK(ProducerOuter_hook, "MGS4: producer probe (outer)")
    }

    // ---- channel census -------------------------------------------------------------------
    //
    // The payload assembler at +68A410 iterates the UI's channel bitmask and emits each
    // channel through the call at +68A9F1, with the channel's descriptor-table entry staged
    // in registers. Logging those per call labels the channels: which carry 96-byte anchor
    // records, which only exist while aiming (the reticle - the dynamic class), which are
    // always present (the static widgets). This is the identity data the whole
    // producer-based positioning plan keys on.
    // Third placement. +68A9F1 guards a rare type and never fired; +68A9C8 turned out to be
    // the per-stream emission loop (eax = GPU stream slot 1/2/4/5, r9 = stride 0xC vertex /
    // 0x10 float4, r10 = running cursor) with the channel not in ebx there. The channel is
    // the function's own first argument, so the probe sits at entry: rcx = channel selector,
    // rdx = payload byte size, and the return address names the submitting caller.
    constexpr uintptr_t kRvaChannelEmitCall = 0x68A410;
    SafetyHookMid ChannelEmit_hook {};
    std::atomic<int> g_ChannelLogBudget { 1500 };

    void ChannelEmitProbe(SafetyHookContext& ctx)
    {
        if (g_ChannelLogBudget.fetch_sub(1) <= 0)
        {
            g_ChannelLogBudget.store(0);
            return;
        }
        uintptr_t returnAddress = 0;
        std::memcpy(&returnAddress, reinterpret_cast<const void*>(ctx.rsp), sizeof(returnAddress));
        const auto base = reinterpret_cast<uintptr_t>(baseModule);
        spdlog::info("MGS4: UI channel: sel={:X} size={:X} ret=+{:X}",
            static_cast<uint32_t>(ctx.rcx), static_cast<uint32_t>(ctx.rdx),
            (returnAddress >= base) ? returnAddress - base : returnAddress);
    }

    void InstallChannelCensus()
    {
        ChannelEmit_hook = safetyhook::create_mid(
            reinterpret_cast<uint8_t*>(baseModule) + kRvaChannelEmitCall, ChannelEmitProbe);
        LOG_HOOK(ChannelEmit_hook, "MGS4: UI channel census")

        // F6 re-arms the budget, so a specific moment (aiming, a menu) can be sampled.
        std::thread([]
            {
                bool wasDown = false;
                while (true)
                {
                    const bool isDown = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
                    if (isDown && !wasDown)
                    {
                        g_ChannelLogBudget.store(1500);
                        spdlog::info("MGS4: UI channel census: re-armed.");
                    }
                    wasDown = isDown;
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            }).detach();
    }

    // ---- LA2 load logging -----------------------------------------------------------------
    //
    // Hooks the game's archive read to log every .la2 UI layout file as it loads, and parses
    // each file's header in memory - version, section offsets, and the layer table with its
    // strcode names - using the structure from the community's 010 template (author "eol").
    // This is the file-and-layer identity map the producer-based positioning keys on.
    //
    // The byte signature for the archive read function is published by cipherxof's
    // MGS4-ModLoader (MIT, github.com/cipherxof/MGS4-ModLoader); the hook and the parsing
    // here are ours. Function shape per the same source:
    //   int32 ArchiveRead(const char* path, void* dest, uint64 destSize, uint64 offset,
    //                     uint64 requestedSize)
    constexpr const char* kArchiveReadSignature =
        "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 49 8B D9 49 8B F8 48 8B F2 E8 D3 F7 FF FF 48 8B D0";

    // ArchiveFileInfo(path, &size): the game's own file-size query, for reads that request
    // "the whole file" with size -1 (the read's return value is a status, not a count).
    // Signature from cipherxof's MGS4-ModLoader (MIT).
    constexpr const char* kArchiveFileInfoSignature =
        "40 53 48 83 EC 20 48 8B DA E8 ? ? ? ? 48 85 C0 74 12 48 8B 08 48 85 C0 48 89 0B 0F 95 C0";
    using ArchiveFileInfoFn = bool(__fastcall*)(const char* path, uint64_t* size);
    ArchiveFileInfoFn g_ArchiveFileInfo = nullptr;

    SafetyHookInline ArchiveRead_hook {};

    uint32_t La2Word(const uint8_t* at, bool bigEndian)
    {
        uint32_t value = 0;
        std::memcpy(&value, at, sizeof(value));
        return bigEndian ? _byteswap_ulong(value) : value;
    }

    void LogLa2Header(const char* path, const uint8_t* data, uint64_t bytes)
    {
        // One header log per file; the game reads large files in chunks.
        static std::mutex seenMutex;
        static std::vector<std::string> seen;
        {
            std::lock_guard lock(seenMutex);
            for (const auto& entry : seen)
            {
                if (entry == path)
                {
                    return;
                }
            }
            if (seen.size() < 256)
            {
                seen.emplace_back(path);
            }
        }

        // The template auto-detects endianness; PS3-era data is big-endian and the port may
        // keep it. Versions run 0x10007..0x10009.
        uint32_t version = La2Word(data, false);
        bool bigEndian = false;
        if (version < 0x10007 || version > 0x10009)
        {
            version = La2Word(data, true);
            bigEndian = true;
        }
        if (version < 0x10007 || version > 0x10009)
        {
            spdlog::info("MGS4: LA2 {}: unrecognised version word {:08X}.", path, La2Word(data, false));
            return;
        }

        const uint32_t ofLayer = (version > 0x10007 && bytes >= 13 * 4)
            ? La2Word(data + 12 * 4, bigEndian) : 0;
        spdlog::info("MGS4: LA2 {}: version {:X}{}, layer table at {:X}.",
            path, version, bigEndian ? " (big-endian)" : "", ofLayer);

        if (ofLayer == 0 || ofLayer + 4 > bytes)
        {
            return;
        }

        const uint32_t layerCount = La2Word(data + ofLayer, bigEndian);
        if (layerCount > 256)
        {
            return;
        }
        for (uint32_t i = 0; i < layerCount && ofLayer + 4 + (i + 1) * 8 <= bytes; i++)
        {
            const uint8_t* entry = data + ofLayer + 4 + i * 8;
            const uint32_t strcode = La2Word(entry, bigEndian);
            uint16_t status = 0;
            uint16_t objects = 0;
            std::memcpy(&status, entry + 4, sizeof(status));
            std::memcpy(&objects, entry + 6, sizeof(objects));
            if (bigEndian)
            {
                status = _byteswap_ushort(status);
                objects = _byteswap_ushort(objects);
            }
            spdlog::info("MGS4: LA2 {}:   layer {} strcode={:08X} status={} objects={}",
                path, i, strcode, status, objects);
        }

        // The authored bounding box, from the Status section's vertex packets. This is what
        // decides whether per-object translation can place a file's content: a single-widget
        // file has a tight box whose position names its screen corner; a composite spanning
        // the whole canvas cannot be moved as one piece.
        //
        // Status stream per the template: a count, then per status a packet list; packets are
        // (u16 type, u16 size) with LA_SCMD_VERTEX = 3 / LA_SCMD_ADDVERT = 4 carrying
        // 12.4 fixed-point x,y pairs in per-unit groups.
        const uint32_t ofStat = La2Word(data + 2 * 4, bigEndian);
        if (ofStat == 0 || ofStat + 4 > bytes)
        {
            return;
        }

        int32_t minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
        uint32_t vertices = 0;

        const uint32_t statusCount = La2Word(data + ofStat, bigEndian);
        uint64_t cursor = ofStat + 4;
        for (uint32_t s = 0; s < statusCount && s < 512 && cursor + 4 <= bytes; s++)
        {
            uint16_t packetCount = 0;
            uint16_t statusSize = 0;
            std::memcpy(&packetCount, data + cursor, 2);
            std::memcpy(&statusSize, data + cursor + 2, 2);
            if (bigEndian)
            {
                packetCount = _byteswap_ushort(packetCount);
                statusSize = _byteswap_ushort(statusSize);
            }
            const uint64_t statusEnd = (std::min<uint64_t>)(cursor + statusSize, bytes);
            uint64_t packet = cursor + 4;
            for (uint16_t p = 0; p < packetCount && packet + 4 <= statusEnd; p++)
            {
                uint16_t type = 0;
                uint16_t size = 0;
                std::memcpy(&type, data + packet, 2);
                std::memcpy(&size, data + packet + 2, 2);
                if (bigEndian)
                {
                    type = _byteswap_ushort(type);
                    size = _byteswap_ushort(size);
                }
                if (size < 4 || packet + size > statusEnd)
                {
                    break;
                }
                if ((type == 3 || type == 4) && size >= 12)
                {
                    // unit count, then (vertexCount, unitNumber) groups of 12.4 pairs.
                    uint64_t at = packet + 8;
                    while (at + 4 <= packet + size)
                    {
                        uint16_t vertexCount = 0;
                        std::memcpy(&vertexCount, data + at, 2);
                        if (bigEndian) { vertexCount = _byteswap_ushort(vertexCount); }
                        if (vertexCount == 0 || vertexCount > 256) { break; }
                        at += 4;
                        for (uint16_t v = 0; v < vertexCount && at + 4 <= packet + size; v++, at += 4)
                        {
                            int16_t fx = 0, fy = 0;
                            std::memcpy(&fx, data + at, 2);
                            std::memcpy(&fy, data + at + 2, 2);
                            if (bigEndian)
                            {
                                fx = static_cast<int16_t>(_byteswap_ushort(static_cast<uint16_t>(fx)));
                                fy = static_cast<int16_t>(_byteswap_ushort(static_cast<uint16_t>(fy)));
                            }
                            const int32_t px = fx / 16, py = fy / 16;
                            minX = (std::min)(minX, px); maxX = (std::max)(maxX, px);
                            minY = (std::min)(minY, py); maxY = (std::max)(maxY, py);
                            vertices++;
                        }
                    }
                }
                packet += size;
            }
            cursor += (statusSize >= 4) ? statusSize : 4;
        }

        if (vertices > 0)
        {
            spdlog::info("MGS4: LA2 {}:   authored bbox ({},{})..({},{})  {} vertices",
                path, minX, minY, maxX, maxY, vertices);
        }
    }

    int32_t LoggedArchiveRead(const char* path, void* destination, uint64_t destinationSize,
        uint64_t offset, uint64_t requestedSize)
    {
        const int32_t result = ArchiveRead_hook.stdcall<int32_t>(
            path, destination, destinationSize, offset, requestedSize);

        if (path)
        {
            const size_t length = std::strlen(path);

            // Where do texture .data files actually flow? Log every .data path seen here,
            // gate-free, so the acquisition route stops being a guess.
            if (bDumpLa2 && length >= 5 && _stricmp(path + length - 5, ".data") == 0)
            {
                static std::atomic<int> dataSeen { 0 };
                if (dataSeen.fetch_add(1) < 300)
                {
                    spdlog::info("MGS4: data via ArchiveRead: {} ({} bytes at {}).", path, requestedSize, offset);
                }
            }

            // Texture pixel data: most txn files are header-only shells whose image bytes
            // live in sibling "<8-hex-fileid>_<index>.data" files. Same dump treatment, with
            // the name pattern checked so unrelated .data files stay out.
            // The texture cache reads request "the whole file" with requestedSize = -1, so
            // the real byte count is this call's own result; a fixed size guard rejected
            // every one of them the first time around.
            uint64_t dataBytes = (requestedSize < (32u << 20)) ? requestedSize : 0;
            if (dataBytes == 0 && g_ArchiveFileInfo)
            {
                uint64_t querySize = 0;
                if (g_ArchiveFileInfo(path, &querySize) && querySize > 0 && querySize < (32u << 20))
                {
                    dataBytes = querySize;
                }
            }
            if (bDumpLa2 && length >= 6 && _stricmp(path + length - 5, ".data") == 0
                && destination && dataBytes > 0 && dataBytes < (32u << 20))
            {
                const char* name = std::strrchr(path, '/');
                name = name ? name + 1 : path;
                bool texturePattern = std::strlen(name) >= 12 && name[8] == '_';
                for (int i = 0; texturePattern && i < 8; i++)
                {
                    texturePattern = std::isxdigit(static_cast<unsigned char>(name[i])) != 0;
                }
                if (texturePattern)
                {
                    char dumpPath[MAX_PATH];
                    std::snprintf(dumpPath, sizeof(dumpPath), "C:/mgspf_tools/txn_dump/%s", name);
                    CreateDirectoryA("C:/mgspf_tools/txn_dump", nullptr);
                    HANDLE file = CreateFileA(dumpPath,
                        (offset == 0) ? GENERIC_WRITE : (GENERIC_WRITE | FILE_APPEND_DATA),
                        0, nullptr, (offset == 0) ? CREATE_ALWAYS : OPEN_ALWAYS, 0, nullptr);
                    if (file != INVALID_HANDLE_VALUE)
                    {
                        SetFilePointer(file, 0, nullptr, FILE_END);
                        DWORD written = 0;
                        WriteFile(file, destination, static_cast<DWORD>(dataBytes), &written, nullptr);
                        CloseHandle(file);
                    }
                }
            }

            // Textures ride along when dumping: the contact sheet needs the .txn files the
            // la2s reference to show real icons instead of bare geometry.
            if (bDumpLa2 && length >= 4 && _stricmp(path + length - 4, ".txn") == 0
                && destination && requestedSize > 0 && requestedSize < (64u << 20))
            {
                const char* name = std::strrchr(path, '/');
                name = name ? name + 1 : path;
                char dumpPath[MAX_PATH];
                std::snprintf(dumpPath, sizeof(dumpPath), "C:/mgspf_tools/txn_dump/%s", name);
                CreateDirectoryA("C:/mgspf_tools/txn_dump", nullptr);
                HANDLE file = CreateFileA(dumpPath,
                    (offset == 0) ? GENERIC_WRITE : (GENERIC_WRITE | FILE_APPEND_DATA),
                    0, nullptr, (offset == 0) ? CREATE_ALWAYS : OPEN_ALWAYS, 0, nullptr);
                if (file != INVALID_HANDLE_VALUE)
                {
                    SetFilePointer(file, 0, nullptr, FILE_END);
                    DWORD written = 0;
                    WriteFile(file, destination, static_cast<DWORD>(requestedSize), &written, nullptr);
                    CloseHandle(file);
                }
            }

            if (length >= 4 && _stricmp(path + length - 4, ".la2") == 0)
            {
                spdlog::info("MGS4: LA2 load: {} ({} bytes at offset {}) -> {:X}.",
                    path, requestedSize, offset, reinterpret_cast<uintptr_t>(destination));

                // Optional dump to disk, so the files can be parsed and previewed outside the
                // game - identification by eye beats identification by bias runs.
                if (bDumpLa2 && destination && requestedSize > 0 && requestedSize < (16u << 20))
                {
                    const char* name = std::strrchr(path, '/');
                    name = name ? name + 1 : path;
                    char dumpPath[MAX_PATH];
                    std::snprintf(dumpPath, sizeof(dumpPath), "C:/mgspf_tools/la2_dump/%s", name);
                    CreateDirectoryA("C:/mgspf_tools/la2_dump", nullptr);
                    HANDLE file = CreateFileA(dumpPath,
                        (offset == 0) ? GENERIC_WRITE : (GENERIC_WRITE | FILE_APPEND_DATA),
                        0, nullptr, (offset == 0) ? CREATE_ALWAYS : OPEN_ALWAYS, 0, nullptr);
                    if (file != INVALID_HANDLE_VALUE)
                    {
                        SetFilePointer(file, 0, nullptr, FILE_END);
                        DWORD written = 0;
                        WriteFile(file, destination, static_cast<DWORD>(requestedSize), &written, nullptr);
                        CloseHandle(file);
                    }
                    else
                    {
                        static std::atomic<int> reported { 0 };
                        if (reported.fetch_add(1) < 3)
                        {
                            spdlog::error("MGS4: LA2 dump: CreateFileA('{}') failed, error {}.",
                                dumpPath, GetLastError());
                        }
                    }
                }
                if (offset == 0 && destination && requestedSize >= 13 * 4)
                {
                    LogLa2Header(path, static_cast<const uint8_t*>(destination), requestedSize);
                }
            }
        }
        return result;
    }

    // The game's slot-resource loader: (data, resourceId, flags, size, destination,
    // slotNameHash, pageId), data pointing at the resource bytes. Signature published by
    // cipherxof's MGS4-ModLoader (MIT); hook ours, log-only for now.
    constexpr const char* kSlotResourceLoadSignature =
        "48 83 EC 48 8B 84 24 ? ? ? ? C6 44 24 38 00 89 44 24 30 8B 44 24 78 89 44 24 28 8B 44 24 70 89 44 24 20 E8 ? ? ? ? 48 83 C4 48 C3";

    SafetyHookInline SlotResourceLoad_hook {};

    int32_t LoggedSlotResourceLoad(void* data, uint32_t resourceId, uint32_t flags,
        uint32_t size, uint32_t destination, uint32_t slotNameHash, uint32_t pageId)
    {
        static std::atomic<int> seen { 0 };
        if (seen.fetch_add(1) < 400)
        {
            spdlog::info("MGS4: slot resource: id={:08X} slot={:08X} page={} size={} flags={:X}",
                resourceId, slotNameHash, pageId, size, flags);
        }
        return SlotResourceLoad_hook.stdcall<int32_t>(data, resourceId, flags, size,
            destination, slotNameHash, pageId);
    }

    void InstallLa2Logging()
    {
        spdlog::info("MGS4: LA2 logging: dump {}.", bDumpLa2 ? "ON" : "off");

        if (bDumpLa2)
        {
            if (uint8_t* info = Memory::PatternScan(baseModule, kArchiveFileInfoSignature, "MGS4: archive file info"))
            {
                g_ArchiveFileInfo = reinterpret_cast<ArchiveFileInfoFn>(info);
            }

            if (uint8_t* at = Memory::PatternScan(baseModule, kSlotResourceLoadSignature, "MGS4: slot resource logging"))
            {
                SlotResourceLoad_hook = safetyhook::create_inline(at,
                    reinterpret_cast<void*>(&LoggedSlotResourceLoad));
                LOG_HOOK(SlotResourceLoad_hook, "MGS4: slot resource logging")
            }
        }
        if (uint8_t* at = Memory::PatternScan(baseModule, kArchiveReadSignature, "MGS4: LA2 logging"))
        {
            ArchiveRead_hook = safetyhook::create_inline(at, reinterpret_cast<void*>(&LoggedArchiveRead));
            LOG_HOOK(ArchiveRead_hook, "MGS4: LA2 logging")
        }
    }

    // ---- native rect emitter logging ------------------------------------------------------
    //
    // The widget backing panels are native solid-rectangle nodes, not textured quads: they
    // pass through the physical-rect emitter, not the layout-object pipeline - which is why
    // no object bias in the live walks could move them. Byte pattern and function shape
    // published by mgs4Ultra120 (MIT-adjacent research, attributed); the hook and use are
    // ours. Shape: uint8_t* emit(uint8_t* dest, u16 x, u16 y, u16 w, u16 h), writing a
    // 16-byte packet. Log-only; the live command "rectlog" re-arms the budget.
    constexpr const char* kRectEmitterSignature =
        "0F B7 44 24 28 66 89 41 0E 48 8D 41 10 C6 01 0A 66 89 51 08 66 44 89 41 0A 66 44 89 49 0C C3";

    SafetyHookInline RectEmitter_hook {};
    std::atomic<int> g_RectLogBudget { 0 };

    uint8_t* LoggedRectEmit(uint8_t* dest, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
    {
        if (g_RectLogBudget.fetch_sub(1) > 0)
        {
            uintptr_t returnAddress = 0;
            std::memcpy(&returnAddress, _AddressOfReturnAddress(), sizeof(returnAddress));
            const auto base = reinterpret_cast<uintptr_t>(baseModule);
            spdlog::info("MGS4: rect emit: ({},{} {}x{}) from +{:X}",
                x, y, w, h, (returnAddress >= base) ? returnAddress - base : returnAddress);
        }
        else
        {
            g_RectLogBudget.store(0);
        }
        return RectEmitter_hook.stdcall<uint8_t*>(dest, x, y, w, h);
    }

    void InstallRectEmitLogging()
    {
        if (uint8_t* at = Memory::PatternScan(baseModule, kRectEmitterSignature, "MGS4: rect emitter"))
        {
            RectEmitter_hook = safetyhook::create_inline(at, reinterpret_cast<void*>(&LoggedRectEmit));
            LOG_HOOK(RectEmitter_hook, "MGS4: rect emitter")
        }
    }

    // ---- live anchor lab ------------------------------------------------------------------
    //
    // The user's idea: stop boot-cycling. A thread polls a command file and rewrites layout
    // object mappings on the running game, so experiments are captures seconds apart instead
    // of runs minutes apart. Commands, one per line, executed then the file is cleared:
    //   bias <index> <dx> <dy>   shift object #index's logical range (logical units)
    //   biascode <hex> <dx> <dy> shift every object with that la2 strcode
    //   reset                    restore every object to the layer-1 centered mapping
    //   list                     log the registry (index, strcode, current range)
    constexpr const char* kLiveCommandFile = "C:/mgspf_tools/live_anchor.txt";

    // Whether a registered object is still the object we recorded. VirtualQuery alone caused
    // a crash in play: a freed-and-reused heap page passes it, and writing 16 bytes into
    // whatever lives there now corrupts the game (EXCEPTION_ACCESS_VIOLATION in engine code,
    // dump 260901_191512). Identity is the recorded strcode at +0x1C plus the two engine
    // pointers every layout object carries at +0x20/+0x28 (module RVAs +437150/+437110,
    // measured from live header dumps). A build change fails the check and degrades to
    // "no anchoring" rather than to corruption.
    bool KnownObjectStillValid(const KnownObject& known)
    {
        MEMORY_BASIC_INFORMATION info {};
        if (!VirtualQuery(reinterpret_cast<void*>(known.object), &info, sizeof(info))
            || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))
            || !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
        {
            return false;
        }
        const auto base = reinterpret_cast<const uint8_t*>(info.BaseAddress);
        if (known.object + 0x30 > reinterpret_cast<uintptr_t>(base) + info.RegionSize)
        {
            return false;
        }
        uint32_t strcode = 0;
        uintptr_t engineA = 0;
        uintptr_t engineB = 0;
        std::memcpy(&strcode, reinterpret_cast<const void*>(known.object + 0x1C), sizeof(strcode));
        std::memcpy(&engineA, reinterpret_cast<const void*>(known.object + 0x20), sizeof(engineA));
        std::memcpy(&engineB, reinterpret_cast<const void*>(known.object + 0x28), sizeof(engineB));
        const auto moduleBase = reinterpret_cast<uintptr_t>(baseModule);
        return strcode == known.strcode
            && engineA == moduleBase + 0x437150
            && engineB == moduleBase + 0x437110;
    }

    void ApplyLiveBias(const KnownObject& known, int dx, int dy)
    {
        if (!KnownObjectStillValid(known))
        {
            return;
        }
        *reinterpret_cast<int32_t*>(known.object + kLayoutLogicalLeft) -= dx;
        *reinterpret_cast<int32_t*>(known.object + kLayoutLogicalRight) -= dx;
        *reinterpret_cast<int32_t*>(known.object + kLayoutLogicalTop) -= dy;
        *reinterpret_cast<int32_t*>(known.object + kLayoutLogicalBottom) -= dy;
        *reinterpret_cast<uint32_t*>(known.object + 0x18) |= 0x220000;
    }

    void ResetLiveObject(const KnownObject& known)
    {
        if (!KnownObjectStillValid(known))
        {
            return;
        }
        *reinterpret_cast<int32_t*>(known.object + kLayoutLogicalLeft) = -g_LiveExtraX;
        *reinterpret_cast<int32_t*>(known.object + kLayoutLogicalTop) = 0;
        *reinterpret_cast<int32_t*>(known.object + kLayoutLogicalRight) = 1280 + g_LiveExtraX;
        *reinterpret_cast<int32_t*>(known.object + kLayoutLogicalBottom) = 720;
        *reinterpret_cast<uint32_t*>(known.object + 0x18) |= 0x220000;
    }

    void StartLiveAnchorLab()
    {
        spdlog::info("MGS4: live anchor lab: polling {}.", kLiveCommandFile);
        std::thread([]
            {
                while (true)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    HANDLE file = CreateFileA(kLiveCommandFile, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                    if (file == INVALID_HANDLE_VALUE)
                    {
                        continue;
                    }
                    char buffer[2048] {};
                    DWORD bytesRead = 0;
                    ReadFile(file, buffer, sizeof(buffer) - 1, &bytesRead, nullptr);
                    CloseHandle(file);
                    if (bytesRead == 0)
                    {
                        continue;
                    }
                    HANDLE clear = CreateFileA(kLiveCommandFile, GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, 0, nullptr);
                    if (clear != INVALID_HANDLE_VALUE) { CloseHandle(clear); }

                    std::lock_guard lock(g_ObjectsMutex);
                    char* context = nullptr;
                    for (char* line = strtok_s(buffer, "\r\n", &context); line;
                         line = strtok_s(nullptr, "\r\n", &context))
                    {
                        int index = 0, dx = 0, dy = 0;
                        unsigned int code = 0;
                        if (sscanf_s(line, "bias %d %d %d", &index, &dx, &dy) == 3)
                        {
                            if (index >= 0 && index < static_cast<int>(g_Objects.size()))
                            {
                                ApplyLiveBias(g_Objects[index], dx, dy);
                                spdlog::info("MGS4: live lab: bias #{} (la2 {:08X}) by ({},{}).",
                                    index, g_Objects[index].strcode, dx, dy);
                            }
                        }
                        else if (sscanf_s(line, "biascode %x %d %d", &code, &dx, &dy) == 3)
                        {
                            for (const auto& known : g_Objects)
                            {
                                if (known.strcode == code) { ApplyLiveBias(known, dx, dy); }
                            }
                            spdlog::info("MGS4: live lab: biascode {:08X} by ({},{}).", code, dx, dy);
                        }
                        else if (std::strncmp(line, "reset", 5) == 0)
                        {
                            for (const auto& known : g_Objects) { ResetLiveObject(known); }
                            spdlog::info("MGS4: live lab: reset {} objects.", g_Objects.size());
                        }
                        else if (std::strncmp(line, "rectlog", 7) == 0)
                        {
                            g_RectLogBudget.store(400);
                            spdlog::info("MGS4: live lab: rect logging armed.");
                        }
                        else if (std::strncmp(line, "pooldump", 8) == 0)
                        {
                            g_PoolDumpBudget.store(1);
                            spdlog::info("MGS4: live lab: pool dump armed.");
                        }
                        else if (std::strncmp(line, "producer", 8) == 0)
                        {
                            g_ProducerProbeBudget.store(120);
                            spdlog::info("MGS4: live lab: producer probe armed.");
                        }
                        else if (std::strncmp(line, "list", 4) == 0)
                        {
                            for (size_t i = 0; i < g_Objects.size(); i++)
                            {
                                spdlog::info("MGS4: live lab: #{} la2 {:08X} obj {:X}",
                                    i, g_Objects[i].strcode, g_Objects[i].object);
                            }
                        }
                    }
                }
            }).detach();
    }

    // ---- runtime corner anchoring ---------------------------------------------------------
    //
    // The bottom widgets' host objects have their mappings rewritten after creation and are
    // recreated whenever the weapon/item selector is used (both measured live), so their
    // anchoring is a periodic absolute re-apply rather than a one-shot conversion write.
    // Rules, each earned by an experiment:
    //   - 002506A9 (ration content), 0029FC73 (ration panel): every instance, left-pinned.
    //     Their latent instances are invisible menus; anchoring them is harmless.
    //   - 00CD1BC3 (weapon content): every instance, right-pinned.
    //   - 0059FFD5 (weapon panel): ONLY instances created immediately after a 0029FC73 -
    //     the file also owns the selector system and the bullseye-adjacent pieces, and
    //     anchoring those broke both.
    // Writes are absolute (idempotent), validated with VirtualQuery, every 400ms.
    void AnchorObjectRange(const KnownObject& known, int align)
    {
        if (!KnownObjectStillValid(known))
        {
            return;
        }
        const int32_t extraX = g_LiveExtraX;
        if (extraX <= 0)
        {
            return;
        }
        auto* left = reinterpret_cast<int32_t*>(known.object + kLayoutLogicalLeft);
        auto* right = reinterpret_cast<int32_t*>(known.object + kLayoutLogicalRight);
        const int32_t wantLeft = -extraX * (1 + align);
        const int32_t wantRight = 1280 + extraX * (1 - align);
        if (*left != wantLeft || *right != wantRight)
        {
            *left = wantLeft;
            *right = wantRight;
            *reinterpret_cast<uint32_t*>(known.object + 0x18) |= 0x220000;
        }
    }

    void StartRuntimeAnchoring()
    {
        std::thread([]
            {
                while (true)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(400));
                    std::lock_guard lock(g_ObjectsMutex);
                    g_Objects.erase(
                        std::remove_if(g_Objects.begin(), g_Objects.end(),
                            [](const KnownObject& known) { return !KnownObjectStillValid(known); }),
                        g_Objects.end());
                    for (const auto& known : g_Objects)
                    {
                        switch (known.strcode)
                        {
                            case 0x002506A9:
                            case 0x0029FC73:
                                AnchorObjectRange(known, -1);
                                break;
                            case 0x00CD1BC3:
                                AnchorObjectRange(known, +1);
                                break;
                            case 0x0059FFD5:
                                if (known.pairedAfterRationPanel)
                                {
                                    AnchorObjectRange(known, +1);
                                }
                                break;
                            default:
                                break;
                        }
                    }
                }
            }).detach();
        spdlog::info("MGS4: runtime corner anchoring active.");
    }

    void ApplyCornerSpread()
    {
        // Corner anchoring itself needs no hook of its own any more - it rides the layout
        // converter above. The upload-helper mid hook serves the producer-watch,
        // ladder-pool and pooldump diagnostics; it installs unconditionally so the live
        // lab can arm them mid-session, and the probe self-gates to a flag check when
        // nothing is armed.
        UploadHelper_hook = safetyhook::create_mid(
            reinterpret_cast<uint8_t*>(baseModule) + kRvaUploadHelper, UploadHelperProbe);
        LOG_HOOK(UploadHelper_hook, "MGS4: UI Aspect producer watch")
    }

    void ApplyFixes()
    {
        if (bLogUiChannels)
        {
            InstallChannelCensus();
        }

        if (bLogLa2Loads)
        {
            InstallLa2Logging();
        }

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
            if (bFixUiAspectCorners && bAspectRuntimeAnchoring)
            {
                StartRuntimeAnchoring();
            }
            StartLiveAnchorLab();
            InstallRectEmitLogging();
            InstallProducerProbe();
        }
    }
}
