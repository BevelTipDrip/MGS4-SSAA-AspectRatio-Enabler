#include "stdafx.h"

#include "aspect_ratio.hpp"

#include "common.hpp"
#include "config.hpp"
#include "logging.hpp"
#include "resolution_scaling_fixes.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
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

        // The object registry only serves the lab (live bias, the corner table's
        // first-instance rule, runtime anchoring). Nothing prunes it in a normal launch, so
        // it is not kept there.
        if (bLabMode)
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
    // in a centered 16:9 area. This layer moves the corner widgets back out to their 16:9
    // margins by translating their NDC anchors - shapes are untouched because size lives in
    // the basis vectors, not the anchor - and leaves everything else where the centered
    // canvas put it.
    //
    // The records are the game's per-frame instance pool: ~200KB, double-buffered, 96-byte
    // anchor blocks appended one at a time by one assembler (+68A410) and uploaded whole
    // through the generic upload helper (+7A8CD0: Map, memcpy, Unmap). Finding the pool
    // took three wrong turns, all recorded in docs/features/aspect-ratio.md: the two
    // "instance array" sites the earlier research identified upload only tiny constant
    // records, and the pool's source pointer rides the stack as the helper's fifth argument,
    // invisible to any register-based probe. The upload hook stays as the mount point for
    // the pool diagnostics; the spread itself is applied per record at the append (see
    // kHudIdentities and PoolAppendProbe).
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

    // Which anchors to shift, by identity. Two positional schemes came before this and both
    // were rejected by the user: "shift everything outside a dead zone" threw world-tracked
    // labels (pickup names, enemy markers) 480px off target the moment they panned across
    // the threshold, and the measured-zone whitelist that replaced it needed a new heuristic
    // for every screen state (wheel bands, a menu signature) and never converged. No
    // position rule separates "widget" from "thing tracking the world", because the world
    // can put an element anywhere.
    //
    // The identity comes from the game's own draw structure (measured 2026-09-02/03 with the
    // emit-attribution probe, see the feature doc): every anchor record reaches the pool
    // through one assembler, and one unwind out of it restores two registers its caller
    // held - rbp, the object doing the drawing, and rsi, the record channel. For the
    // gameplay HUD the owner is one static object (kRvaHudOwner) and the channel is a fixed
    // index per widget and per selector slot: the same widget keeps the same channel with
    // the wheels closed, either wheel open, and across boots. The pause menu draws through
    // heap-allocated owners on the dynamic channels 1-4 - the same numbers the battery bar
    // uses under the HUD owner - so the key is the pair, not the channel alone.
    //
    // Treatment is a constant per side (1 - 1/factor, see the shift derivation below), and
    // everything not in this table - menus, the CAUTION banner, the reticle, world labels,
    // any widget not measured yet - stays exactly where the centered canvas put it. A
    // missing widget is a cosmetic gap and a one-line addition here, never a wrong jump.
    constexpr uintptr_t kRvaHudOwner = 0x1DE3290;   // frame-1 rbp for every gameplay HUD record

    // The widget itself, by the element strcode its anchor-emitting nodes carry (opcode 0x0D,
    // dword at node+8 - the same key the world labels use). The channel alone was the key
    // until hot dump 02 (2026-09-03): the load screen draws its save thumbnails through the
    // HUD owner on channels 3-6, and channel 4 is also the battery bar's, so the third
    // thumbnail went left with the battery. The strcode tells them apart - 0x000D12D5 is the
    // battery wherever it is drawn, 0x00075ED4 a thumbnail - and no channel number needs to
    // mean anything on its own. Which channels each widget was measured on is kept for the
    // record: 0x1D weapon panel (and the wheel slots 0x13/15/17/19/1B, the panel at the row's
    // end), 0x1E item panel (and the item wheel 0x0E/10/12/14/16/18/1A/1C), 0x21 life bar,
    // 0x23 camo ring, 0x04 battery bar; chanbias confirmed by the user's read of the captures.
    struct HudIdentity
    {
        uint32_t strcode;
        int8_t side;        // -1 left edge, +1 right edge
        const char* what;
    };
    constexpr HudIdentity kHudIdentities[] = {
        { 0x00FF6177, +1, "weapon panel" },         // also every weapon wheel slot
        { 0x0093929C, +1, "weapon icon" },
        { 0x00F1D8B7, -1, "item panel / Solid Eye" },   // also every item wheel slot
        { 0x0085D6E9, -1, "life bar" },
        { 0x0064D23C, +1, "camo ring" },
        { 0x000D12D5, -1, "battery bar" },          // sits under the item panel; (-0.539,-0.911)
    };

    // A widget's channel also carries records with no node of their own - the ammo and life
    // ticks (+BCFEA, opcode 0x14), the +BCEF3 pairs - which go where the widget goes. So a
    // record that names its widget sets the channel's treatment for the frame, and a record
    // that names nothing follows it: this frame's if the widget has already been named, last
    // frame's for the records a widget emits before its first named node (the life bar's ticks
    // lead its panel). The channel is one widget's for as long as the widget exists, so a
    // stale treatment outlives it by one frame at most, and a channel nobody names - the
    // load screen's - shifts nothing.
    struct ChannelTreatment
    {
        float dx;
        bool named;
    };

    // The selector overlay is one script under one strcode (0x00077AEB on every one of its
    // anchor nodes, item and weapon wheel alike): a centre-anchored channel that exists only
    // while a wheel is open and carries the fullscreen dim backdrop, the centred item/weapon
    // text, and the slot plates (the solid selected block and the translucent boxes) that
    // must travel with their wheel. One strcode, three treatments - so the key goes one
    // level deeper. A second unwind restores the interpreter position the record was drawn
    // for (frame-2 rbx) and the channel's script block (frame-1 r14); their difference is
    // the element's offset inside the script, constant across frames and boots (runs 14/15,
    // 2026-09-03: the ordinal sweep moved exactly one element per offset). Everything in
    // the script not listed here - the text - stays centred.
    //
    // The channel number is deliberately not part of the key: the overlay was 0x20 in every
    // measured boot, but the gameplay widgets' channels came up one higher after a boot
    // that went through the main menu (0x24 for the camo ring instead of 0x23, and so on),
    // so a channel index is an allocation order, not an identity.
    constexpr uint32_t kSelectorStrcode = 0x00077AEB;

    struct ElementSide
    {
        uint32_t strcode;   // opcode 0x0D node strcode, as for kHudIdentities
        uint32_t elem;      // frame-2 rbx - frame-1 r14
        int8_t side;
        const char* what;
    };
    constexpr ElementSide kHudElements[] = {
        { kSelectorStrcode, 0x1950, -1, "item wheel slot plates" },       // text +0x3B38/+0x3C08
        { kSelectorStrcode, 0x2410, +1, "weapon wheel slot plates" },
    };

    // The wheel backdrops are the opposite case: a translucent dim plate authored to cover
    // the whole 1280 canvas, which on the centered canvas covers only the 16:9 area and
    // leaves the side strips undimmed (hot dump 04, 2026-09-03: "needs to be extended").
    // A plate has no detail to distort, so it is stretched about the screen centre by the
    // spread factor - basis x and origin x both scaled - which maps the 16:9 area onto the
    // full output exactly (hot dumps 07-09, 2026-09-03: basis 0.00117 -> 0.00156 in the
    // record, both wheels dimmed edge to edge). Nothing else in the script is touched.
    struct ElementStretch
    {
        uint32_t strcode;
        uint32_t elem;
        const char* what;
    };
    constexpr ElementStretch kHudStretched[] = {
        { kSelectorStrcode, 0x10F8, "item wheel backdrop" },
        { kSelectorStrcode, 0x1BB8, "weapon wheel backdrop" },
    };

    // World-tracked elements: labels the game pins to a projected 3D point (the pickup
    // label's name plate and bracket). Their channel is allocated per label instance, so
    // the key is the element itself: every anchor-emitting node measured (run 17,
    // 2026-09-03) is opcode 0x0D with the element's strcode in the dword at node+8, and the
    // label's five anchors - the centre first, then the four bracket corners - all carry
    // 0x0066AF8A. The 3D projection already uses the real aspect, so the centre's NDC x is
    // right for the full output width, but the HUD canvas maps it onto the centred 16:9
    // area: the plate lands at 480 + (x+1)*1440 px instead of (x+1)*1920, an error of x*480
    // px that grows toward the edges. Multiplying the centre by the spread factor lands it
    // on the object; the corners are then translated by the same delta so the bracket keeps
    // its native size (the vertical axis needs nothing - the canvas height is the output
    // height).
    struct WorldElement
    {
        uint32_t strcode;   // dword at frame-2 rbx + 8 under opcode 0x0D
        const char* what;
    };
    constexpr WorldElement kWorldElements[] = {
        { 0x0066AF8A, "pickup label" },
        { 0x00860C0D, "stat block" },   // Solid Eye enemy/friendly block: one anchor + tick bar, channels 0x4F/0x50 (run 19)
        { 0x004FBED3, "targeting icon" },   // Solid Eye NV target marker, channels 0x52/0x53 (hot dumps 07/08);
                                            // its screen position also needs the reticle truncation fix, see graphics_settings.cpp
    };

    const WorldElement* WorldElementEntry(uint32_t strcode)
    {
        if (strcode == 0) { return nullptr; }
        for (const auto& entry : kWorldElements)
        {
            if (entry.strcode == strcode) { return &entry; }
        }
        return nullptr;
    }

    const ElementSide* HudElementEntry(uint32_t strcode, uintptr_t elem)
    {
        if (strcode == 0) { return nullptr; }
        for (const auto& entry : kHudElements)
        {
            if (entry.strcode == strcode && entry.elem == elem) { return &entry; }
        }
        return nullptr;
    }

    const ElementStretch* HudStretchEntry(uint32_t strcode, uintptr_t elem)
    {
        if (strcode == 0) { return nullptr; }
        for (const auto& entry : kHudStretched)
        {
            if (entry.strcode == strcode && entry.elem == elem) { return &entry; }
        }
        return nullptr;
    }

    const HudIdentity* HudIdentityEntry(uint32_t strcode)
    {
        if (strcode == 0) { return nullptr; }
        for (const auto& entry : kHudIdentities)
        {
            if (entry.strcode == strcode) { return &entry; }
        }
        return nullptr;
    }

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

    // The shift is a constant per side - deliberately NOT a function of each anchor's exact
    // position. The first version scaled every anchor by the spread factor, grouped by a
    // draw-order proximity heuristic, and the user's comparison shots showed what any
    // position-dependent delta does inside a widget: the reticle's corner dots spread into
    // a rectangle, the weapon silhouette slid out of its box, the compass lost its bullseye
    // and its N drifted. A widget only survives if every one of its parts moves by exactly
    // the same amount.
    //
    // The constant is 1 - 1/factor per side, and that number is not arbitrary: compressed
    // NDC times full output width equals safe-canvas NDC times safe width, so this shift
    // preserves each widget's *pixel* distance from the screen edge exactly as it is on a
    // 16:9 output. Corners land at their 16:9 margins; everything inside a widget stays
    // pixel-rigid; an open selector wheel moves with its junction slot and arrives at the
    // corner as one piece.
    //
    // Where it is applied: at the pool *append*, one record at a time, in PoolAppendProbe -
    // not on the assembled pool at upload. The upload sees the whole 200KB ping-pong buffer,
    // of which only the first cursor's worth is this frame's; the rest is stale from
    // earlier frames (measured: ~150 live blocks of ~210 present with the HUD closed), and
    // the positional pass that walked it took stale menu records for an open menu. The
    // append sees exactly one live record with its identity one unwind away.
    float SideShift()
    {
        const float factor = HorizontalSpreadFactor();
        return factor == 1.0f ? 0.0f : 1.0f - 1.0f / factor;
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
    // Upload census ("uploads <n>"): every buffer upload through the helper, not just the
    // ones carrying anchor blocks - the selector's backing plates sit at fixed screen
    // positions whatever the anchor channels do, so they travel in some other buffer.
    std::atomic<int> g_UploadLogBudget { 0 };
    // Raw dump ("bufdump"): the next upload from each of the three per-frame sites (instance
    // pool, index buffer, vertex buffer) written whole to C:\mgspf_tools\lab3\buf\ for
    // offline searching - the plates' screen rectangle has to be in one of them.
    std::atomic<int> g_BufDumpMask { 0 };
    std::atomic<int> g_BufDumpSeq { 0 };

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

    extern std::atomic<int> g_SiteDumpFrames;
    void DumpSites(const uint8_t* pool, size_t bytes, uintptr_t source);

    void UploadHelperProbe(SafetyHookContext& ctx)
    {
        // The helper is one of the game's hottest paths (every buffer upload). The hook
        // now installs unconditionally so the lab can arm diagnostics mid-session, so the
        // probe must cost one flag check when nothing is armed. The corner spread itself
        // no longer lives here (see PoolAppendProbe); this hook is diagnostics only.
        if (!ResolutionScalingFixes::bWatchAnchorProducers
            && !bAspectTestLadder && g_PoolDumpBudget.load(std::memory_order_relaxed) <= 0
            && g_SiteDumpFrames.load(std::memory_order_relaxed) <= 0
            && g_UploadLogBudget.load(std::memory_order_relaxed) <= 0
            && g_BufDumpMask.load(std::memory_order_relaxed) == 0)
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

        if (g_UploadLogBudget.load(std::memory_order_relaxed) > 0
            && g_UploadLogBudget.fetch_sub(1) > 0)
        {
            std::string floats, hex;
            MEMORY_BASIC_INFORMATION info {};
            const bool readable = source
                && VirtualQuery(reinterpret_cast<void*>(source), &info, sizeof(info))
                && info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (readable)
            {
                const size_t n = (std::min<size_t>)(ctx.r9, 64);
                for (size_t i = 0; i + 4 <= n; i += 4)
                {
                    float f;
                    std::memcpy(&f, reinterpret_cast<const void*>(source + i), sizeof(f));
                    floats += std::format("{}{:.4g}", i ? "," : "", f);
                }
                for (size_t i = 0; i < n; i++)
                {
                    hex += std::format("{:02X}", *reinterpret_cast<const uint8_t*>(source + i));
                }
            }
            spdlog::info("MGS4: uploads: ret=+{:X} size={} off={} src={:X} f=[{}] hex={}",
                retRva, ctx.r9, ctx.r8, source, floats, hex);
        }

        if (g_BufDumpMask.load(std::memory_order_relaxed) != 0 && source && ctx.r9 <= (8u << 20))
        {
            const int bit = retRva == kRvaInstancePoolCaller ? 1 : retRva == 0x7A946F ? 2 : retRva == 0x7A94BF ? 4 : 0;
            if (bit && (g_BufDumpMask.fetch_and(~bit) & bit))
            {
                MEMORY_BASIC_INFORMATION info {};
                if (VirtualQuery(reinterpret_cast<void*>(source), &info, sizeof(info))
                    && info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                {
                    const auto path = std::format("C:\\mgspf_tools\\lab3\\buf\\{:02}_{:X}.bin",
                        g_BufDumpSeq.load(), retRva);
                    if (FILE* f = std::fopen(path.c_str(), "wb"))
                    {
                        std::fwrite(reinterpret_cast<const void*>(source), 1, ctx.r9, f);
                        std::fclose(f);
                    }
                    spdlog::info("MGS4: bufdump: {} ({} bytes, off={}).", path, ctx.r9, ctx.r8);
                }
            }
        }

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

        // Emit attribution: label the instance pool's anchor blocks with the append events
        // that wrote them (armed by the "sites" live command, see PoolAppendProbe).
        if (spread && g_SiteDumpFrames.load() > 0)
        {
            DumpSites(reinterpret_cast<const uint8_t*>(source),
                (std::min<uint64_t>)(ctx.r9, 1u << 20), source);
        }

        // The pool spread that used to live here is gone: corner anchoring now happens
        // per-object in the layout converter, keyed on each object's la2 strcode. This hook
        // remains solely as the mount point for the diagnostics above.
    }

    // ---- emit attribution ("sites") ----------------------------------------------------------
    //
    // Identity from the game's own call structure. Every append into the instance pool is one
    // memcpy at +68A933 (rcx = pool cursor, rdx = the assembler's frame, r8 = size). While
    // armed, each append records where in the pool it landed and the chain of game frames
    // that asked for it - unwound with the executable's own unwind tables from the hook
    // context, because RtlCaptureStackBackTrace cannot see past a mid hook's trampoline -
    // plus the first known layout object found in those frames' stack slots. The next upload
    // of the pool then labels every anchor block with its chain and object: la2-driven quads
    // name their layout object outright; programmatic ones (the wheel chrome, which follows
    // no la2 range) name the code path that draws them. Both are static identities, which is
    // what the per-object treatment table keys on.
    constexpr uintptr_t kRvaPoolAppendCall = 0x68A933;
    SafetyHookMid PoolAppend_hook {};

    constexpr int kSiteFrames = 12;
    constexpr int kSiteRegFrames = 7;   // frames whose restored non-volatile registers are kept
    constexpr int kSiteRegs = 8;        // rbx rbp rsi rdi r12 r13 r14 r15
    constexpr uintptr_t kSiteObjectSpan = 0x1000;   // "inside a known object" tolerance
    struct AppendEvent
    {
        uintptr_t dst;
        uint32_t size;
        uint8_t frameCount;
        uint32_t frames[kSiteFrames];   // module RVAs, innermost first
        uintptr_t object;               // first known layout object seen in a frame's slots
        uint32_t objectStrcode;
        int objectFrame;
        uintptr_t objectDelta;          // where inside the object the pointer landed
        // Non-volatile registers as the unwinder restores them on entering frame i, i.e. the
        // values the code at frames[i] held: a widget's this pointer lives here if anywhere.
        uintptr_t regs[kSiteRegFrames][kSiteRegs];
        uint8_t head[32];               // the record's first bytes (non-anchor records are decoded from this)
        uint8_t node[64];               // bytes at frame-2 rbx, the per-element node the record was drawn for
        bool nodeRead;
    };
    std::mutex g_AppendMutex;
    std::vector<AppendEvent> g_AppendEvents;
    std::vector<KnownObject> g_SiteObjects;   // registry snapshot taken when the probe is armed
    std::atomic<int> g_SiteDumpFrames { 0 };

    // Channel-keyed live bias (the "chanbias" command). The channel index rides down to the
    // assembler in rsi (measured 2026-09-02: it partitions every anchor cluster by widget
    // and by wheel slot, and menus sit on the dynamic channels 1-4), and the record being
    // appended still lives in the assembler's frame, so an anchor block can be shifted in
    // place before the memcpy and the result is on screen the same frame.
    // 128: the Solid Eye stat blocks take channels 0x4F/0x50 (runs 18/19, 2026-09-03), so
    // the dynamic range runs past 64.
    constexpr int kChannelCount = 128;
    float g_ChannelBias[kChannelCount] {};
    std::atomic<bool> g_ChannelBiasActive { false };
    // Ordinal bias ("chanord"): only the n-th record a channel appends per frame. Counted on
    // the render thread only, so the counter needs no atomics.
    std::atomic<int> g_OrdChannel { -1 };
    std::atomic<int> g_OrdIndex { 0 };
    float g_OrdDx = 0.0f;
    int g_OrdCount = 0;

    void BuildAppendContext(const SafetyHookContext& ctx, CONTEXT& c)
    {
        const auto base = reinterpret_cast<uintptr_t>(baseModule);
        c = CONTEXT {};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        // Unwind from the call's return address, not the call itself: the hook's jmp now
        // sits at +68A933 and the unwinder reads a jmp at Rip as a tail-call epilogue.
        c.Rip = base + kRvaPoolAppendCall + 5;
        c.Rsp = ctx.rsp; c.Rbp = ctx.rbp; c.Rbx = ctx.rbx; c.Rsi = ctx.rsi; c.Rdi = ctx.rdi;
        c.R12 = ctx.r12; c.R13 = ctx.r13; c.R14 = ctx.r14; c.R15 = ctx.r15;
        c.Rax = ctx.rax; c.Rcx = ctx.rcx; c.Rdx = ctx.rdx; c.R8 = ctx.r8; c.R9 = ctx.r9;
        c.R10 = ctx.r10; c.R11 = ctx.r11;
    }

    // One unwind out of the assembler restores the registers its caller held: rbp is the
    // drawing object (the owner), rsi the channel index. channel is -1 when the value is
    // not a plausible index.
    struct AppendIdentity
    {
        uintptr_t owner;
        int channel;
        bool hasElem;
        uintptr_t elem;     // frame-2 rbx - frame-1 r14, see kHudElements
        uint32_t strcode;   // the node's element strcode, 0 when the node is not opcode 0x0D
    };

    bool UnwindOnce(CONTEXT& c)
    {
        DWORD64 imageBase = 0;
        auto* entry = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
        if (!entry)
        {
            return false;
        }
        PVOID handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, entry, &c, &handlerData, &establisher, nullptr);
        return c.Rip != 0;
    }

    AppendIdentity IdentifyAppend(const SafetyHookContext& ctx, bool wantElem)
    {
        CONTEXT c;
        BuildAppendContext(ctx, c);
        if (!UnwindOnce(c))
        {
            return { 0, -1, false, 0 };
        }
        AppendIdentity id { static_cast<uintptr_t>(c.Rbp),
            c.Rsi < static_cast<DWORD64>(kChannelCount) ? static_cast<int>(c.Rsi) : -1, false, 0, 0 };
        if (wantElem && id.channel >= 0)
        {
            const uintptr_t script = static_cast<uintptr_t>(c.R14);
            if (UnwindOnce(c))
            {
                const uintptr_t node = static_cast<uintptr_t>(c.Rbx);
                id.hasElem = true;
                id.elem = node - script;
                // The node the interpreter is executing: it has just read this opcode and
                // its operands out of the script block, so the same bytes are readable here
                // as long as the position really is inside the block r14 points at.
                if (script > 0x10000 && id.elem < 0x10000)
                {
                    const auto* bytes = reinterpret_cast<const uint8_t*>(node);
                    if (bytes[0] == 0x0D)
                    {
                        std::memcpy(&id.strcode, bytes + 8, sizeof(id.strcode));
                    }
                }
            }
        }
        return id;
    }

    // The corner spread proper, plus the lab's per-channel bias: the record about to be
    // appended is still in the assembler's frame (rdx), so its anchor is moved in place.
    // Rotated quads (the compass arrow, the weapon silhouette) pass the relaxed signature;
    // under a known owner and channel the structural test only has to reject non-anchor
    // 96-byte records, and every 96-byte record measured on the HUD channels was an anchor.
    std::atomic<uint64_t> g_ShiftedIdentitiesLogged { 0 };
    std::atomic<uint64_t> g_ShiftedElementsLogged { 0 };
    std::atomic<uint64_t> g_StretchedElementsLogged { 0 };
    std::atomic<uint64_t> g_WorldElementsLogged { 0 };

    // The per-channel treatment, this frame and last. A widget's named records set it - a
    // corner widget to its side's constant, a world label to the correction its reference
    // needs - and its unnamed records read it. World tracking: the first world-element
    // record a channel appends in a frame is the label's reference (the pickup label's
    // centre, +0x298, leads its four corners); its correction is remembered and applied to
    // the rest of the group, so the bracket moves as one piece. The stat block emits ten
    // records - two +BCEF3 and eight tick pips - *before* its reference (run 20,
    // 2026-09-03: they stayed at the native x while the rest moved), and the life bar's
    // ticks lead its panel the same way, so a record that arrives before its channel is
    // named this frame takes last frame's treatment. The channel is dedicated to the widget
    // while it exists and the widget names it every frame, so a stale treatment can outlive
    // its widget by one frame at most. Render thread only. The world lab switch: 0 off, 1
    // translate the group by the reference's correction (default), 2 scale every anchor -
    // the wrong treatment for a fixed-size bracket, kept for comparison.
    std::atomic<int> g_WorldMode { 1 };
    uintptr_t g_FrameLastDst = 0;
    ChannelTreatment g_ChannelNow[kChannelCount] {};
    ChannelTreatment g_ChannelLast[kChannelCount] {};

    // A new frame starts at the base of the other pool (or below the last append).
    bool NewFrameAt(uintptr_t dst)
    {
        const bool fresh = dst < g_FrameLastDst || dst > g_FrameLastDst + 0x10000;
        g_FrameLastDst = dst;
        return fresh;
    }

    float ChannelDx(int channel)
    {
        if (g_ChannelNow[channel].named) { return g_ChannelNow[channel].dx; }
        if (g_ChannelLast[channel].named) { return g_ChannelLast[channel].dx; }
        return 0.0f;
    }

    void ShiftAppend(SafetyHookContext& ctx, bool spread, bool bias)
    {
        if ((ctx.r8 & 0xFFFF) != kAnchorBlockSize)
        {
            return;
        }
        auto* src = reinterpret_cast<uint8_t*>(ctx.rdx);
        if (!RelaxedAnchorBlock(src))
        {
            return;
        }

        const AppendIdentity id = IdentifyAppend(ctx, spread);
        if (id.channel < 0)
        {
            return;
        }
        if (NewFrameAt(ctx.rcx))
        {
            g_OrdCount = 0;
            std::memcpy(g_ChannelLast, g_ChannelNow, sizeof(g_ChannelLast));
            std::memset(g_ChannelNow, 0, sizeof(g_ChannelNow));
        }

        float dx = 0.0f;
        if (spread && id.owner == reinterpret_cast<uintptr_t>(baseModule) + kRvaHudOwner)
        {
            ChannelTreatment& now = g_ChannelNow[id.channel];
            if (const WorldElement* world = WorldElementEntry(id.strcode))
            {
                const int worldMode = g_WorldMode.load(std::memory_order_relaxed);
                if (worldMode != 0 && (worldMode == 2 || !now.named))
                {
                    float x = 0.0f;
                    std::memcpy(&x, src + kAnchorBlockV8, sizeof(x));
                    now = { x * (HorizontalSpreadFactor() - 1.0f), true };
                    const uint64_t logBit = 1ull << (world - kWorldElements);
                    if ((g_WorldElementsLogged.fetch_or(logBit) & logBit) == 0)
                    {
                        spdlog::info("MGS4: UI Aspect: world element {:08X} ({}) on channel 0x{:02X}: reference element +{:X}, x {:.4f} -> {:.4f}.",
                            world->strcode, world->what, id.channel, id.elem, x, x + now.dx);
                    }
                }
            }
            else if (const HudIdentity* identity = HudIdentityEntry(id.strcode))
            {
                now = { static_cast<float>(identity->side) * SideShift(), true };
                const uint64_t logBit = 1ull << (identity - kHudIdentities);
                if ((g_ShiftedIdentitiesLogged.fetch_or(logBit) & logBit) == 0)
                {
                    spdlog::info("MGS4: UI Aspect: element {:08X} ({}) on channel 0x{:02X} anchored {} by {:.4f}.",
                        identity->strcode, identity->what, id.channel, identity->side < 0 ? "left" : "right", SideShift());
                }
            }

            // The selector overlay is the one case keyed by element offset: its script holds
            // both wheels' backdrops, plates and text under a single strcode, and each needs
            // its own treatment. None of them names the channel - the text stays centred.
            const ElementSide* element = id.hasElem ? HudElementEntry(id.strcode, id.elem) : nullptr;
            if (element)
            {
                dx += static_cast<float>(element->side) * SideShift();
                const uint64_t bit = 1ull << (element - kHudElements);
                if ((g_ShiftedElementsLogged.fetch_or(bit) & bit) == 0)
                {
                    spdlog::info("MGS4: UI Aspect: element {:08X}+{:X} ({}) on channel 0x{:02X} anchored {} by {:.4f}.",
                        element->strcode, id.elem, element->what, id.channel, element->side < 0 ? "left" : "right", SideShift());
                }
            }
            else if (const ElementStretch* stretch = id.hasElem ? HudStretchEntry(id.strcode, id.elem) : nullptr)
            {
                // Stretched about the screen centre: x basis and x origin scaled together,
                // so the plate's 16:9 span becomes the full output span.
                const float factor = HorizontalSpreadFactor();
                float basisX = 0.0f, originX = 0.0f;
                std::memcpy(&basisX, src + kAnchorBlockV6, sizeof(basisX));
                std::memcpy(&originX, src + kAnchorBlockV8, sizeof(originX));
                basisX *= factor;
                originX *= factor;
                std::memcpy(src + kAnchorBlockV6, &basisX, sizeof(basisX));
                std::memcpy(src + kAnchorBlockV8, &originX, sizeof(originX));
                const uint64_t bit = 1ull << (stretch - kHudStretched);
                if ((g_StretchedElementsLogged.fetch_or(bit) & bit) == 0)
                {
                    spdlog::info("MGS4: UI Aspect: element {:08X}+{:X} ({}) on channel 0x{:02X} stretched by {:.4f}.",
                        stretch->strcode, id.elem, stretch->what, id.channel, factor);
                }
            }
            else
            {
                dx += ChannelDx(id.channel);
            }
        }
        if (bias)
        {
            dx += g_ChannelBias[id.channel];
            const int ordChannel = g_OrdChannel.load(std::memory_order_relaxed);
            if (ordChannel >= 0)
            {
                if (id.channel == ordChannel)
                {
                    if (g_OrdCount == g_OrdIndex.load(std::memory_order_relaxed))
                    {
                        dx += g_OrdDx;
                    }
                    g_OrdCount++;
                }
            }
        }
        if (dx != 0.0f)
        {
            float x = 0.0f;
            std::memcpy(&x, src + kAnchorBlockV8, sizeof(x));
            x += dx;
            std::memcpy(src + kAnchorBlockV8, &x, sizeof(x));
        }
    }

    void PoolAppendProbe(SafetyHookContext& ctx)
    {
        const bool sites = g_SiteDumpFrames.load(std::memory_order_relaxed) > 0;
        const bool bias = g_ChannelBiasActive.load(std::memory_order_relaxed);
        const bool spread = bAspectPoolSpread && SideShift() != 0.0f;   // strict no-op at 16:9
        if (!sites && !bias && !spread)
        {
            return;
        }

        if (bias || spread)
        {
            ShiftAppend(ctx, spread, bias);
        }

        if (!sites)
        {
            return;
        }

        AppendEvent ev {};
        ev.dst = ctx.rcx;
        ev.size = static_cast<uint32_t>(ctx.r8 & 0xFFFF);   // movzx r8d, si at +68A92B
        std::memcpy(ev.head, reinterpret_cast<const void*>(ctx.rdx), (std::min<size_t>)(ev.size, sizeof(ev.head)));

        const auto base = reinterpret_cast<uintptr_t>(baseModule);
        CONTEXT c;
        BuildAppendContext(ctx, c);

        const auto noteObject = [&](uintptr_t value, int frame) {
            if (ev.object) { return; }
            for (const auto& known : g_SiteObjects)
            {
                if (value >= known.object && value < known.object + kSiteObjectSpan)
                {
                    ev.object = known.object;
                    ev.objectStrcode = known.strcode;
                    ev.objectFrame = frame;
                    ev.objectDelta = value - known.object;
                    return;
                }
            }
        };

        {
            const uintptr_t regs0[kSiteRegs] { c.Rbx, c.Rbp, c.Rsi, c.Rdi, c.R12, c.R13, c.R14, c.R15 };
            for (int r = 0; r < kSiteRegs; r++) { ev.regs[0][r] = regs0[r]; noteObject(regs0[r], 0); }
        }

        for (int i = 0; i < kSiteFrames; i++)
        {
            DWORD64 imageBase = 0;
            const DWORD64 frameRsp = c.Rsp;
            auto* entry = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!entry)
            {
                // Leaf frame: the return address sits at [rsp].
                std::memcpy(&c.Rip, reinterpret_cast<const void*>(c.Rsp), sizeof(c.Rip));
                c.Rsp += 8;
            }
            else
            {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, entry, &c,
                    &handlerData, &establisher, nullptr);
            }
            if (!c.Rip)
            {
                break;
            }

            // This frame's slots, for a pointer into a registered layout object.
            if (c.Rsp > frameRsp)
            {
                const DWORD64 slots = (std::min<DWORD64>)((c.Rsp - frameRsp) / 8, 128);
                for (DWORD64 s = 0; s < slots && !ev.object; s++)
                {
                    uintptr_t value = 0;
                    std::memcpy(&value, reinterpret_cast<const void*>(frameRsp + s * 8), sizeof(value));
                    noteObject(value, i);
                }
            }

            const bool inGame = c.Rip >= base && c.Rip < base + 0x30000000;
            ev.frames[ev.frameCount] = inGame ? static_cast<uint32_t>(c.Rip - base) : 0xFFFFFFFFu;
            const int frame = ev.frameCount + 1;   // the frame the restored registers belong to
            if (frame < kSiteRegFrames)
            {
                const uintptr_t regs[kSiteRegs] { c.Rbx, c.Rbp, c.Rsi, c.Rdi, c.R12, c.R13, c.R14, c.R15 };
                for (int r = 0; r < kSiteRegs; r++)
                {
                    ev.regs[frame][r] = regs[r];
                    noteObject(regs[r], frame);
                }
            }
            ev.frameCount++;
            if (!inGame)
            {
                break;
            }
        }

        // The element node: frame-2 rbx on every anchor record measured so far (a heap
        // pointer, per record, distinct even between records on one channel).
        {
            const uintptr_t node = ev.regs[2][0];
            MEMORY_BASIC_INFORMATION info {};
            if (node > 0x10000 && VirtualQuery(reinterpret_cast<void*>(node), &info, sizeof(info))
                && info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))
                && node + sizeof(ev.node) <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize)
            {
                std::memcpy(ev.node, reinterpret_cast<const void*>(node), sizeof(ev.node));
                ev.nodeRead = true;
            }
        }

        std::lock_guard lock(g_AppendMutex);
        if (g_AppendEvents.size() < 16384)
        {
            g_AppendEvents.push_back(ev);
        }
    }

    void InstallSiteAttribution()
    {
        // call memcpy ; mov esi, [rbp+1A8h] - the append and the instruction after it.
        static constexpr uint8_t kExpected[] { 0xE8, 0x58, 0xA5, 0x69, 0x00, 0x8B, 0xB5, 0xA8, 0x01, 0x00, 0x00 };
        auto* at = reinterpret_cast<uint8_t*>(baseModule) + kRvaPoolAppendCall;
        if (std::memcmp(at, kExpected, sizeof(kExpected)) != 0)
        {
            spdlog::error("MGS4: pool append attribution: +{:X} is not the expected call - not hooking.",
                kRvaPoolAppendCall);
            return;
        }
        PoolAppend_hook = safetyhook::create_mid(at, PoolAppendProbe);
        LOG_HOOK(PoolAppend_hook, "MGS4: pool append attribution")
    }

    // The upload-side half: label each anchor block of the pool being uploaded with the
    // append event that wrote it (the latest event covering its address, since the pools
    // ping-pong and an older frame's events for the same addresses may still be queued).
    void DumpSites(const uint8_t* pool, size_t bytes, uintptr_t source)
    {
        std::vector<AppendEvent> events;
        {
            std::lock_guard lock(g_AppendMutex);
            events.swap(g_AppendEvents);
        }
        const int remaining = g_SiteDumpFrames.fetch_sub(1) - 1;
        const float factor = HorizontalSpreadFactor();
        spdlog::info("MGS4: sites: begin pool={:X} size={} events={} factor={:.4f} frames-left={}",
            source, bytes, events.size(), factor, remaining);
        // The game uploads the whole pool every frame but only appends up to its cursor;
        // everything past the last append is stale content from earlier frames (measured
        // 2026-09-02: 178 appends covering pool+0..+0x1BB0 against 211 anchor blocks in the
        // 200 KB upload). Only the live region is attributed.
        size_t inside = 0, sizeSum = 0, sz96 = 0;
        uintptr_t liveEnd = source;
        for (const auto& e : events)
        {
            if (e.dst >= source && e.dst < source + bytes)
            {
                inside++;
                sizeSum += e.size;
                if (e.size == 96) { sz96++; }
                liveEnd = (std::max)(liveEnd, static_cast<uintptr_t>(e.dst + e.size));
            }
        }
        spdlog::info("MGS4: sites: events inside={} bytes={} size96={} live=+0..+{:X}",
            inside, sizeSum, sz96, liveEnd - source);

        int logged = 0;
        int unattributed = 0;
        int stale = 0;
        size_t offset = 0;
        bool inRun = false;
        while (offset + kAnchorBlockSize <= bytes && logged < 600)
        {
            if (source + offset >= liveEnd)
            {
                // Count the stale blocks so the census still reconciles with pooldump.
                if (PlausibleAnchorBlock(pool + offset)) { stale++; offset += kAnchorBlockSize; }
                else { offset += 16; }
                continue;
            }
            const bool strict = PlausibleAnchorBlock(pool + offset);
            if (!strict && !(inRun && RelaxedAnchorBlock(pool + offset)))
            {
                inRun = false;
                offset += 16;
                continue;
            }
            inRun = true;
            float v6[2] {}, v7[2] {}, v8[2] {};
            std::memcpy(v6, pool + offset + kAnchorBlockV6, sizeof(v6));
            std::memcpy(v7, pool + offset + kAnchorBlockV7, sizeof(v7));
            std::memcpy(v8, pool + offset + kAnchorBlockV8, sizeof(v8));

            const uintptr_t address = source + offset;
            const AppendEvent* ev = nullptr;
            for (auto it = events.rbegin(); it != events.rend(); ++it)
            {
                if (address >= it->dst && address < it->dst + it->size)
                {
                    ev = &*it;
                    break;
                }
            }

            std::string chain;
            std::string regs;
            if (ev)
            {
                for (int i = 0; i < ev->frameCount; i++)
                {
                    chain += (i ? "<+" : "+");
                    chain += (ev->frames[i] == 0xFFFFFFFFu) ? std::string("ext") : std::format("{:X}", ev->frames[i]);
                }
                // regs: per frame "f<i>:" then bx bp si di 12 13 14 15 as bare hex.
                const int regFrames = (std::min)(static_cast<int>(ev->frameCount) + 1, kSiteRegFrames);
                for (int f = 0; f < regFrames; f++)
                {
                    regs += std::format(" f{}:", f);
                    for (int r = 0; r < kSiteRegs; r++)
                    {
                        regs += std::format("{}{:X}", r ? "," : "", ev->regs[f][r]);
                    }
                }
            }
            else
            {
                unattributed++;
            }

            std::string node;
            if (ev && ev->nodeRead)
            {
                for (size_t i = 0; i < sizeof(ev->node); i++)
                {
                    node += std::format("{}{:02X}", (i && i % 8 == 0) ? " " : "", ev->node[i]);
                }
            }

            // v6/v7 are the basis: the quad's extent in clip units, so a backing plate's
            // width is readable straight off the line (Issue 04, the wheel backdrops).
            spdlog::info("MGS4: sites: +{:05X} v8=({:.4f},{:.4f}) norm=({:.4f},{:.4f}) v6=({:.5f},{:.5f}) v7=({:.5f},{:.5f}){} {} {}{} node={}",
                offset, v8[0], v8[1], v8[0] * factor, v8[1], v6[0], v6[1], v7[0], v7[1], strict ? "" : " r",
                ev ? std::format("rec@+{:X}/{}", address - ev->dst, ev->size) : "no-event",
                ev ? (ev->object ? std::format("obj={:08X}+{:X}@f{} {}", ev->objectStrcode, ev->objectDelta, ev->objectFrame, chain)
                                 : ("obj=- " + chain))
                   : "",
                regs, node);
            logged++;
            offset += kAnchorBlockSize;
        }
        spdlog::info("MGS4: sites: end, {} live blocks, {} unattributed, {} stale past the cursor.",
            logged, unattributed, stale);

        // Everything that is not a 96-byte anchor record, in pool order: the selector's
        // backing plates were on no anchor channel (item-wheel diff sweep, 2026-09-02), so
        // they must travel in one of these. Head bytes as floats and as hex, with the frame-1
        // owner/channel the anchor path uses for identity.
        int others = 0;
        for (const auto& e : events)
        {
            if (e.dst < source || e.dst >= source + bytes || e.size == 96 || others >= 400)
            {
                continue;
            }
            others++;
            std::string floats, hex;
            const size_t n = (std::min<size_t>)(e.size, sizeof(e.head));
            for (size_t i = 0; i + 4 <= n; i += 4)
            {
                float f;
                std::memcpy(&f, e.head + i, sizeof(f));
                floats += std::format("{}{:.4g}", i ? "," : "", f);
            }
            for (size_t i = 0; i < n; i++)
            {
                hex += std::format("{:02X}", e.head[i]);
            }
            std::string chain;
            for (int i = 0; i < e.frameCount && i < 6; i++)
            {
                chain += (i ? "<+" : "+");
                chain += (e.frames[i] == 0xFFFFFFFFu) ? std::string("ext") : std::format("{:X}", e.frames[i]);
            }
            spdlog::info("MGS4: sites: rec +{:05X}/{} owner={:X} ch={:X} f=[{}] hex={} {}",
                e.dst - source, e.size, e.regs[1][1], e.regs[1][2], floats, hex, chain);
        }
        spdlog::info("MGS4: sites: {} non-anchor records listed.", others);
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
    // Per-site budgets: the wrapper fires far more often than the outer frame, and a
    // shared budget drained entirely at the wrapper before the outer ever logged.
    std::atomic<int> g_ProbeBudgetWrapper { 0 };
    std::atomic<int> g_ProbeBudgetOuter { 0 };

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

    // Whether a value is plausibly a heap pointer worth dereferencing.
    bool PlausibleHeapPointer(uintptr_t value)
    {
        return value > 0x10000 && value < 0x7FFFFFFFFFFF && (value & 0x7) == 0;
    }

    void ProbeProducerSite(const char* site, const SafetyHookContext& ctx)
    {
        struct Candidate { const char* name; uintptr_t value; };
        const Candidate registers[] = {
            { "rcx", ctx.rcx }, { "rdx", ctx.rdx }, { "r8", ctx.r8 }, { "r9", ctx.r9 },
            { "rbx", ctx.rbx }, { "rbp", ctx.rbp }, { "rsi", ctx.rsi }, { "rdi", ctx.rdi },
            { "r10", ctx.r10 }, { "r11", ctx.r11 }, { "r12", ctx.r12 }, { "r13", ctx.r13 },
            { "r14", ctx.r14 }, { "r15", ctx.r15 },
        };

        uintptr_t returnAddress = 0;
        std::memcpy(&returnAddress, reinterpret_cast<const void*>(ctx.rsp), sizeof(returnAddress));
        const auto base = reinterpret_cast<uintptr_t>(baseModule);
        const uintptr_t retRva = (returnAddress >= base) ? returnAddress - base : returnAddress;

        bool found = false;
        for (const auto& reg : registers)
        {
            uint32_t strcode = 0;
            if (LooksLikeLayoutObject(reg.value, &strcode))
            {
                spdlog::info("MGS4: producer probe [{}]: {} = obj {:X} la2 {:08X} ret=+{:X}",
                    site, reg.name, reg.value, strcode, retRva);
                found = true;
                continue;
            }
            // One level of indirection: a register may hold the element record whose
            // fields include the owning object (the wrapper's r9 carries such a pointer).
            if (!PlausibleHeapPointer(reg.value))
            {
                continue;
            }
            MEMORY_BASIC_INFORMATION info {};
            if (!VirtualQuery(reinterpret_cast<void*>(reg.value), &info, sizeof(info))
                || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            {
                continue;
            }
            const auto regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
            for (int field = 0; field < 12 && reg.value + field * 8 + 8 <= regionEnd; field++)
            {
                uintptr_t inner = 0;
                std::memcpy(&inner, reinterpret_cast<const void*>(reg.value + field * 8), sizeof(inner));
                if (LooksLikeLayoutObject(inner, &strcode))
                {
                    spdlog::info("MGS4: producer probe [{}]: [{}+{:X}] = obj {:X} la2 {:08X} ret=+{:X}",
                        site, reg.name, field * 8, inner, strcode, retRva);
                    found = true;
                }
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
                spdlog::info("MGS4: producer probe [{}]: rsp+{:X} = obj {:X} la2 {:08X} ret=+{:X}",
                    site, slot * 8, value, strcode, retRva);
                found = true;
            }
        }

        if (!found)
        {
            spdlog::info("MGS4: producer probe [{}]: no object (rcx={:X} rdx={:X} r8={:X} r9={:X} ret=+{:X})",
                site, ctx.rcx, ctx.rdx, ctx.r8, ctx.r9, retRva);
        }
    }

    void ProducerWrapperProbe(SafetyHookContext& ctx)
    {
        if (g_ProbeBudgetWrapper.load(std::memory_order_relaxed) <= 0) { return; }
        if (g_ProbeBudgetWrapper.fetch_sub(1) <= 0) { g_ProbeBudgetWrapper.store(0); return; }
        ProbeProducerSite("wrapper+666470", ctx);
    }

    void ProducerOuterProbe(SafetyHookContext& ctx)
    {
        if (g_ProbeBudgetOuter.load(std::memory_order_relaxed) <= 0) { return; }
        if (g_ProbeBudgetOuter.fetch_sub(1) <= 0) { g_ProbeBudgetOuter.store(0); return; }
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
                        else if (std::strncmp(line, "bufdump", 7) == 0)
                        {
                            g_BufDumpSeq.fetch_add(1);
                            g_BufDumpMask.store(7);
                            spdlog::info("MGS4: live lab: buffer dump {} armed.", g_BufDumpSeq.load());
                        }
                        else if (std::strncmp(line, "uploads", 7) == 0)
                        {
                            const int n = static_cast<int>(std::strtol(line + 7, nullptr, 10));
                            g_UploadLogBudget.store(n > 0 ? n : 300);
                            spdlog::info("MGS4: live lab: upload census armed ({} uploads).",
                                g_UploadLogBudget.load());
                        }
                        else if (std::strncmp(line, "producer", 8) == 0)
                        {
                            g_ProbeBudgetWrapper.store(60);
                            g_ProbeBudgetOuter.store(60);
                            spdlog::info("MGS4: live lab: producer probe armed (60 per site).");
                        }
                        else if (std::strncmp(line, "chanbias", 8) == 0)
                        {
                            // chanbias <channel> <dx>   (channel decimal or 0x.., dx in NDC)
                            char* end = nullptr;
                            const long channel = std::strtol(line + 8, &end, 0);
                            const float dx = std::strtof(end, nullptr);
                            if (channel >= 0 && channel < kChannelCount)
                            {
                                g_ChannelBias[channel] = dx;
                                bool any = false;
                                for (float v : g_ChannelBias) { any = any || v != 0.0f; }
                                g_ChannelBiasActive.store(any);
                                spdlog::info("MGS4: live lab: channel {} (0x{:X}) bias {:+.4f}; active={}",
                                    channel, channel, dx, any);
                            }
                        }
                        else if (std::strncmp(line, "spread", 6) == 0)
                        {
                            // spread 0|1 - the identity-keyed corner spread, live.
                            bAspectPoolSpread = std::strtol(line + 6, nullptr, 0) != 0;
                            spdlog::info("MGS4: live lab: pool spread {}.", bAspectPoolSpread ? "on" : "off");
                        }
                        else if (std::strncmp(line, "world", 5) == 0)
                        {
                            // world 0|1|2 - world-tracked labels: off, translate group, scale each.
                            g_WorldMode.store(static_cast<int>(std::strtol(line + 5, nullptr, 0)));
                            spdlog::info("MGS4: live lab: world mode {}.", g_WorldMode.load());
                        }
                        else if (std::strncmp(line, "chanord", 7) == 0)
                        {
                            // chanord <channel> <ordinal> <dx> - bias only the n-th record a
                            // channel appends each frame, to tell the records of one channel apart.
                            char* end = nullptr;
                            const long channel = std::strtol(line + 7, &end, 0);
                            const long ordinal = std::strtol(end, &end, 0);
                            g_OrdDx = std::strtof(end, nullptr);
                            g_OrdIndex.store(static_cast<int>(ordinal));
                            g_OrdChannel.store(channel >= 0 && channel < kChannelCount ? static_cast<int>(channel) : -1);
                            g_ChannelBiasActive.store(true);
                            spdlog::info("MGS4: live lab: channel 0x{:X} record #{} bias {:+.4f}.",
                                channel, ordinal, g_OrdDx);
                        }
                        else if (std::strncmp(line, "chanreset", 9) == 0)
                        {
                            for (float& v : g_ChannelBias) { v = 0.0f; }
                            g_OrdChannel.store(-1);
                            g_ChannelBiasActive.store(false);
                            spdlog::info("MGS4: live lab: channel biases cleared.");
                        }
                        else if (std::strncmp(line, "sites", 5) == 0)
                        {
                            // Snapshot the registry for the append probe's stack scan, drop
                            // any stale events, then arm one pool dump's worth of frames.
                            {
                                std::lock_guard appendLock(g_AppendMutex);
                                g_SiteObjects = g_Objects;
                                g_AppendEvents.clear();
                                g_AppendEvents.reserve(4096);
                            }
                            g_SiteDumpFrames.store(2);
                            spdlog::info("MGS4: live lab: emit attribution armed ({} objects).",
                                g_SiteObjects.size());
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
        // A normal launch gets exactly two hooks: the layout converter (layer 1, the
        // undistorted centered canvas) and the pool append (layer 2, the identity-keyed
        // corner spread). Everything else here is research instrumentation - the upload-helper
        // probe, the live command thread, the rect/producer/channel logging - whose switches
        // only exist in the lab settings file and which must not run outside a lab boot.
        if (bLabMode)
        {
            if (bLogUiChannels)
            {
                InstallChannelCensus();
            }

            if (bLogLa2Loads)
            {
                InstallLa2Logging();
            }
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
        if (!UiLayoutConverter_hook)
        {
            return;
        }

        if (bLabMode)
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

        // The spread's own hook. Its probe self-gates to a flag check when neither the
        // spread nor a lab diagnostic is armed, and the spread is a strict no-op at 16:9.
        if (bAspectPoolSpread || bLabMode)
        {
            InstallSiteAttribution();
        }
    }
}
