#include "pch.hpp"

#include "aspect_ratio.hpp"

#include "game.hpp"
#include "mem.hpp"
#include "config.hpp"
#include "log.hpp"
#include "render_pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <dxgi1_4.h>
#include <format>
#include <intrin.h>
#include <mutex>

#pragma intrinsic(_ReturnAddress)

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
#if MGS4E_LAB_BUILD
    // ---- perf census (lab, Debug Logging) ---------------------------------------------------
    //
    // The user reports a less stable frame rate at 21:9 than at 16:9 with the Expanded HUD,
    // even though 16:9 renders more pixels (2026-09-05). Everything of ours that runs per
    // frame is a strict no-op at 16:9 and does real work otherwise, so the first question is
    // whether the difference is ours. Once a second, a line with the present-to-present
    // frame times (avg / p99 / max) and the time spent inside each hook. Read two sessions
    // against each other, same scene, one per aspect.
    struct PerfCensus
    {
        std::atomic<uint64_t> appendCalls { 0 }, appendTicks { 0 }, appendMax { 0 };
        std::atomic<uint64_t> uploadCalls { 0 }, uploadTicks { 0 };
        std::atomic<uint64_t> convertCalls { 0 }, convertTicks { 0 };
        std::atomic<uint64_t> bandTicks { 0 };
        std::atomic<uint64_t> unwinds { 0 }, queries { 0 };
        std::mutex mutex;
        std::vector<double> frames;   // present-to-present, ms, this window
        uint64_t lastPresent = 0;
        uint64_t windowStart = 0;
    };
    PerfCensus g_Perf;

    uint64_t PerfTicks()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return static_cast<uint64_t>(t.QuadPart);
    }

    double PerfMs(uint64_t ticks)
    {
        static const double msPerTick = []
            {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                return 1000.0 / static_cast<double>(f.QuadPart);
            }();
        return static_cast<double>(ticks) * msPerTick;
    }

    struct PerfScope
    {
        std::atomic<uint64_t>& calls;
        std::atomic<uint64_t>& sum;
        std::atomic<uint64_t>* max;
        uint64_t t0;
        PerfScope(std::atomic<uint64_t>& c, std::atomic<uint64_t>& s, std::atomic<uint64_t>* m) : calls(c), sum(s), max(m), t0(PerfTicks()) {}
        ~PerfScope()
        {
            const uint64_t d = PerfTicks() - t0;
            calls.fetch_add(1, std::memory_order_relaxed);
            sum.fetch_add(d, std::memory_order_relaxed);
            if (max)
            {
                uint64_t m = max->load(std::memory_order_relaxed);
                while (d > m && !max->compare_exchange_weak(m, d, std::memory_order_relaxed)) {}
            }
        }
    };

    // Called once per present. Logs and resets after each full second.
    void PerfPresent()
    {
        if (!mgs4e::log::Verbose()) { return; }
        const uint64_t now = PerfTicks();
        std::lock_guard lock(g_Perf.mutex);
        if (g_Perf.lastPresent) { g_Perf.frames.push_back(PerfMs(now - g_Perf.lastPresent)); }
        g_Perf.lastPresent = now;
        if (!g_Perf.windowStart) { g_Perf.windowStart = now; return; }
        if (PerfMs(now - g_Perf.windowStart) < 1000.0) { return; }

        auto& fr = g_Perf.frames;
        std::sort(fr.begin(), fr.end());
        const size_t n = fr.size();
        double sum = 0.0;
        for (double v : fr) { sum += v; }
        const double avg = n ? sum / static_cast<double>(n) : 0.0;
        const double p50 = n ? fr[n / 2] : 0.0;
        const double p99 = n ? fr[(std::min)(n - 1, static_cast<size_t>(n * 99 / 100))] : 0.0;
        const double mx = n ? fr.back() : 0.0;

        const int w = RenderPipeline::pWindowResX ? *RenderPipeline::pWindowResX : 0;
        const int h = RenderPipeline::pWindowResY ? *RenderPipeline::pWindowResY : 0;

        spdlog::info("MGS4: perf: {}x{} {} presents | frame ms avg {:.2f} p50 {:.2f} p99 {:.2f} max {:.2f} | "
                     "append {} calls {:.2f} ms max {:.3f} ms unwinds {} vq {} | upload {} calls {:.2f} ms | "
                     "convert {} calls {:.2f} ms | bands {:.2f} ms",
            w, h, n, avg, p50, p99, mx,
            g_Perf.appendCalls.load(), PerfMs(g_Perf.appendTicks.load()), PerfMs(g_Perf.appendMax.load()),
            g_Perf.unwinds.load(), g_Perf.queries.load(),
            g_Perf.uploadCalls.load(), PerfMs(g_Perf.uploadTicks.load()),
            g_Perf.convertCalls.load(), PerfMs(g_Perf.convertTicks.load()),
            PerfMs(g_Perf.bandTicks.load()));

        fr.clear();
        g_Perf.windowStart = now;
        for (auto* a : { &g_Perf.appendCalls, &g_Perf.appendTicks, &g_Perf.appendMax, &g_Perf.uploadCalls, &g_Perf.uploadTicks,
                         &g_Perf.convertCalls, &g_Perf.convertTicks, &g_Perf.bandTicks, &g_Perf.unwinds, &g_Perf.queries })
        {
            a->store(0, std::memory_order_relaxed);
        }
    }
#endif

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
#if MGS4E_LAB_BUILD
        PerfScope perf(g_Perf.convertCalls, g_Perf.convertTicks, nullptr);
#endif
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
        std::memcpy(&outputWidth, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kRvaWindowSizeX), sizeof(outputWidth));
        std::memcpy(&outputHeight, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kRvaWindowSizeY), sizeof(outputHeight));

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
        if (mgs4e::config::LabMode())
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
            int32_t phys[4] {};
            std::memcpy(phys, reinterpret_cast<const void*>(object + 0x200), sizeof(phys));
            spdlog::info("MGS4: UI Aspect: {}x{} output, physical {} {} {} {}, logical range {}..{} x {}..{}.",
                outputWidth, outputHeight, phys[0], phys[1], phys[2], phys[3], -extraX, 1280 + extraX, -extraY, 720 + extraY);
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
    // The ring is two objects: the Solid Eye's, measured first, and the smaller compass the
    // game draws in its place with the Solid Eye unequipped (channel 0x22, hot dump 15,
    // 2026-09-03 - it sat at the centred position with the Solid Eye's ring tabled alone).
    //
    // Each widget names both the horizontal edge it belongs to (for outputs wider than 16:9)
    // and the vertical one (for outputs taller than 16:9, where the same rule is rotated:
    // layer 1 letterboxes the interface into a centred 16:9 band and the corner widgets
    // move up or down to the real output edges). Only the axis with spare room ever shifts,
    // so at 21:9 the vertical side is inert and at 4:3 the horizontal one is.
    struct HudIdentity
    {
        uint32_t strcode;
        int8_t side;        // -1 left edge, +1 right edge
        int8_t sideY;       // -1 bottom edge, +1 top edge (NDC y is up)
        const char* what;
    };
    constexpr HudIdentity kHudIdentities[] = {
        { 0x00FF6177, +1, -1, "weapon panel" },         // also every weapon wheel slot
        { 0x0093929C, +1, -1, "weapon icon" },
        { 0x00F1D8B7, -1, -1, "item panel / Solid Eye" },   // also every item wheel slot
        { 0x0085D6E9, -1, +1, "life bar" },
        { 0x0064D23C, +1, +1, "camo ring (Solid Eye)" },
        { 0x00925509, +1, +1, "camo ring (compass)" },  // the plain ring the Solid Eye replaces; hot dump 15
        { 0x000D12D5, -1, -1, "battery bar" },          // sits under the item panel; (-0.539,-0.911)
        // Top-centre: never needed a horizontal side, so it went untabled until 4:3 showed it
        // sitting 270 px below the top edge (user, 2026-09-04). Channel 0x26 in the chanbias
        // census; the node was in hot dumps 04-15 at (0, 0.8111).
        { 0x0071FC3E,  0, +1, "alert / caution banner" },
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
        float dy;
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
        int8_t sideY;       // the wheels hang off the bottom boxes, so both go down at 4:3
        const char* what;
    };
    constexpr ElementSide kHudElements[] = {
        { kSelectorStrcode, 0x1950, -1, -1, "item wheel slot plates" },       // text +0x3B38/+0x3C08
        { kSelectorStrcode, 0x2410, +1, -1, "weapon wheel slot plates" },
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
    // its native size. On a wide output the vertical axis needs nothing - the canvas height
    // is the output height - and on a tall output it is the other way round: the canvas
    // width is the output width and the letterboxed band is what misplaces the label
    // vertically, so the same correction runs on y with the vertical factor.
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
        std::memcpy(&outputWidth, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kRvaWindowSizeX), sizeof(outputWidth));
        std::memcpy(&outputHeight, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kRvaWindowSizeY), sizeof(outputHeight));

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

    // The same number for an output taller than 16:9 - 4:3, say - where layer 1 grows the
    // logical range vertically instead (extraY) and the interface sits letterboxed in a
    // centred 16:9 band. Exactly one of the two factors is ever above 1.0: at 4:3 this is
    // 4/3 and the horizontal one is 1.0, at 21:9 the other way round, at 16:9 both are 1.0.
    float VerticalSpreadFactor()
    {
        int32_t outputWidth = 0;
        int32_t outputHeight = 0;
        std::memcpy(&outputWidth, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kRvaWindowSizeX), sizeof(outputWidth));
        std::memcpy(&outputHeight, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kRvaWindowSizeY), sizeof(outputHeight));

        if (outputWidth < 320 || outputHeight < 240 || outputWidth > 16384 || outputHeight > 16384)
        {
            return 1.0f;
        }

        const int64_t safeHeight = static_cast<int64_t>(outputWidth) * 9 / 16;
        if (safeHeight <= 0 || safeHeight >= outputHeight)
        {
            return 1.0f;
        }
        return static_cast<float>(outputHeight) / static_cast<float>(safeHeight);
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

    // The 4:3 case is the same rule rotated: the widgets that sit at the top and bottom of
    // the 16:9 band move up and down to the real output edges by 1 - 1/factorY, keeping
    // their 16:9 pixel margins from the top and bottom of the screen. At 2880x2160 that is
    // 0.25 NDC = 270 px per side. Never both: a wider output has SideShiftY() == 0 and a
    // taller one has SideShift() == 0, so a widget's (sideX, sideY) pair only ever moves it
    // along the axis the output actually has spare room on.
    float SideShiftY()
    {
        const float factor = VerticalSpreadFactor();
        return factor == 1.0f ? 0.0f : 1.0f - 1.0f / factor;
    }

    // The edge a shift is toward, for the log: the horizontal pair on a wide output, the
    // vertical pair on a tall one.
    const char* ShiftEdgeName(int8_t sideX, int8_t sideY)
    {
        if (SideShift() != 0.0f) { return sideX < 0 ? "left" : sideX > 0 ? "right" : "centre"; }
        return sideY < 0 ? "bottom" : sideY > 0 ? "top" : "centre";
    }

    float ActiveShift()
    {
        return SideShift() != 0.0f ? SideShift() : SideShiftY();
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
                RenderPipeline::ArmProducerWatch(slotAddress,
                    "the static producer (ration box anchor)");
            }

            expected = false;
            if (std::fabs(normX) < 0.04f && v8[1] > 0.10f && v8[1] < 0.40f
                && dynamicArmed.compare_exchange_strong(expected, true))
            {
                RenderPipeline::ArmProducerWatch(slotAddress,
                    "the dynamic producer (reticle anchor)");
            }

            offset += kAnchorBlockSize;
        }
    }

    extern std::atomic<int> g_SiteDumpFrames;
    void DumpSites(const uint8_t* pool, size_t bytes, uintptr_t source);

    void UploadHelperProbe(SafetyHookContext& ctx)
    {
#if MGS4E_LAB_BUILD
        PerfScope perf(g_Perf.uploadCalls, g_Perf.uploadTicks, nullptr);
#endif
        // The helper is one of the game's hottest paths (every buffer upload). The hook
        // now installs unconditionally so the lab can arm diagnostics mid-session, so the
        // probe must cost one flag check when nothing is armed. The corner spread itself
        // no longer lives here (see PoolAppendProbe); this hook is diagnostics only.
        if (!RenderPipeline::bWatchAnchorProducers
            && !bAspectTestLadder && g_PoolDumpBudget.load(std::memory_order_relaxed) <= 0
            && g_SiteDumpFrames.load(std::memory_order_relaxed) <= 0
            && g_UploadLogBudget.load(std::memory_order_relaxed) <= 0
            && g_BufDumpMask.load(std::memory_order_relaxed) == 0)
        {
            return;
        }

        uintptr_t returnAddress = 0;
        std::memcpy(&returnAddress, reinterpret_cast<const void*>(ctx.rsp), sizeof(returnAddress));
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
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

#if MGS4E_LAB_BUILD
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
#endif

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
        if (RenderPipeline::bWatchAnchorProducers && spread)
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
    float g_ChannelBiasY[kChannelCount] {};   // "chanbias <ch> <dx> <dy>": the 4:3 measurements bias vertically
    std::atomic<bool> g_ChannelBiasActive { false };
    // Ordinal bias ("chanord"): only the n-th record a channel appends per frame. Counted on
    // the render thread only, so the counter needs no atomics.
    std::atomic<int> g_OrdChannel { -1 };
    std::atomic<int> g_OrdIndex { 0 };
    float g_OrdDx = 0.0f;
    int g_OrdCount = 0;

    void BuildAppendContext(const SafetyHookContext& ctx, CONTEXT& c)
    {
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
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
        uint8_t opcode;     // lab (screenlog): the node's opcode and its operand qword, for
        uint64_t operand;   // nodes with no strcode
    };

    bool UnwindOnce(CONTEXT& c)
    {
#if MGS4E_LAB_BUILD
        g_Perf.unwinds.fetch_add(1, std::memory_order_relaxed);
#endif
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

    // The 12 bytes of a script node, or nothing. SEH in a function with no C++ objects.
    //
    // AR-013 (2026-09-05): the perf census put this hook at ~240 ms of every second in
    // gameplay and ~900 ms with an in-game menu up at 21:9, and the cost tracked the
    // VirtualQuery count - ~0.4 ms per call here, against ~0.25 us for a whole
    // RtlVirtualUnwind. The query guarded the read of nodes outside the script block, which
    // the menus' heap owners always are. A guarded read costs nothing when the page is
    // mapped and one caught fault when it is not.
    bool ReadNodeBytes(uintptr_t node, uint8_t* out)
    {
        __try
        {
            std::memcpy(out, reinterpret_cast<const void*>(node), 12);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    AppendIdentity IdentifyAppend(const SafetyHookContext& ctx, bool wantElem)
    {
        CONTEXT c;
        BuildAppendContext(ctx, c);
        if (!UnwindOnce(c))
        {
            return { 0, -1, false, 0, 0, 0, 0 };
        }
        AppendIdentity id { static_cast<uintptr_t>(c.Rbp),
            c.Rsi < static_cast<DWORD64>(kChannelCount) ? static_cast<int>(c.Rsi) : -1, false, 0, 0, 0, 0 };
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
                // as long as the position really is inside the block r14 points at. The
                // menus' heap owners (the pause menu, the codec) run their nodes from
                // outside that block - node - r14 was 0xA48630 on the pause menu, census
                // 2026-09-04 - so for those the read is guarded (AR-013).
                uint8_t bytes[12] {};
                bool readable = false;
                if (script > 0x10000 && id.elem < 0x10000)
                {
                    std::memcpy(bytes, reinterpret_cast<const void*>(node), sizeof(bytes));
                    readable = true;
                }
                else if (node > 0x10000)
                {
#if MGS4E_LAB_BUILD
                    g_Perf.queries.fetch_add(1, std::memory_order_relaxed);
#endif
                    readable = ReadNodeBytes(node, bytes);
                }
                if (readable)
                {
                    id.opcode = bytes[0];
                    std::memcpy(&id.operand, bytes + 8, sizeof(id.operand));
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

    const ChannelTreatment* ChannelCarry(int channel)
    {
        if (g_ChannelNow[channel].named) { return &g_ChannelNow[channel]; }
        if (g_ChannelLast[channel].named) { return &g_ChannelLast[channel]; }
        return nullptr;
    }

    // The letterbox's menu gate (AR-010): screens that keep the 16:9 frame besides
    // cutscenes, named by an element their layout script draws every frame they are up
    // and on no other screen (census 2026-09-04, sites dumps read against gameplay and the
    // BASIC CONTROLS overlay). The pause menu and the codec share one la2 object
    // (001997E2) and these two elements. PRESS ANY BUTTON is the HUD owner's channel 1 and
    // its object stays appended (parked above the screen, channel 0xB) on the main menu
    // list, whose own rows are 0052133A on channel 0xA. The same structural style as the
    // cutscene tick: the pool append is the signal, and BandsWanted() reads how long ago a
    // listed element was last appended.
    struct ScreenIdentity
    {
        uint32_t strcode;
        const char* what;
    };
    constexpr ScreenIdentity kScreenIdentities[] = {
        { 0x005F9476, "pause menu / codec" },
        { 0x001AAC76, "pause menu / codec" },
        { 0x00042FAA, "title (press any button)" },
        { 0x0052133A, "main menu list" },
        // The fade plate is 16:9-sized, so on a wider output the world shows past its
        // sides while it is up (title boot, 2026-09-04); bands there cover exactly that.
        { 0x004FC96A, "fade plate" },
    };
    std::atomic<ULONGLONG> g_ScreenBuiltTick { 0 };      // last time a listed element was appended
    std::atomic<uint32_t> g_ScreenLastStrcode { 0 };
    std::atomic<uint64_t> g_ScreenIdentitiesLogged { 0 };

    bool NoteScreenElement(uint32_t strcode)
    {
        if (!strcode) { return false; }
        for (const auto& screen : kScreenIdentities)
        {
            if (screen.strcode != strcode) { continue; }
            g_ScreenBuiltTick.store(GetTickCount64(), std::memory_order_relaxed);
            g_ScreenLastStrcode.store(strcode, std::memory_order_relaxed);
            const uint64_t bit = 1ull << (&screen - kScreenIdentities);
            if ((g_ScreenIdentitiesLogged.fetch_or(bit) & bit) == 0)
            {
                spdlog::info("MGS4: letterbox: screen element {:08X} ({}) seen; bands follow it.", screen.strcode, screen.what);
            }
            return true;
        }
        return false;
    }

    // The screen gate is a latch on pool frames, not a timer: bands stay while the last
    // completed pool frame (and the one being built) contained a listed element. The title
    // boot showed why a timer is not enough - while the graveyard stage loads behind the
    // fade plate the UI stalls for stretches longer than any sensible window (gate_t2,
    // 2026-09-04: 23% of presents unbanded over 10 s with the plate up, the world visible
    // past its sides in the capture), yet the pool is simply not rebuilt during a stall, so
    // the latch holds. The 150 ms tick stays OR'd in to bridge the moment a frame boundary
    // is seen before that frame's element arrives.
    uintptr_t g_ScreenFrameLastDst = 0;
    std::atomic<bool> g_ScreenInThisFrame { false };
    std::atomic<bool> g_ScreenInLastFrame { false };

    bool ScreenFrameNote(uintptr_t dst, bool listed)
    {
        const bool fresh = dst < g_ScreenFrameLastDst || dst > g_ScreenFrameLastDst + 0x10000;
        g_ScreenFrameLastDst = dst;
        if (fresh)
        {
            g_ScreenInLastFrame.store(g_ScreenInThisFrame.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_ScreenInThisFrame.store(false, std::memory_order_relaxed);
        }
        if (listed)
        {
            g_ScreenInThisFrame.store(true, std::memory_order_relaxed);
        }
        return fresh;
    }

    bool ScreenLive()
    {
        return g_ScreenInThisFrame.load(std::memory_order_relaxed)
            || g_ScreenInLastFrame.load(std::memory_order_relaxed)
            || GetTickCount64() - g_ScreenBuiltTick.load(std::memory_order_relaxed) <= 150;
    }

    // The boot phase: bands are forced from the first present until the player is in the
    // game, which is where the tabled screens leave gaps that no element names. Between the
    // fade plate and PRESS ANY BUTTON the title fades through ~2 s of an untabled black quad
    // with the world visible past it (fade_g43, 2026-09-04), and the loaded-save plate (Outer
    // Haven skull, PRESS ANY BUTTON) is a full-window quad without a strcode that the HUD
    // draws over at the window edges once the stage is up (load43e/g).
    //
    // The pool at +68A933 is a change stream, not a per-frame draw list: a static screen
    // appends nothing. The skull plate shows that plainly - after the loading-done burst
    // (the level's HUD channels 12-43 initialised in one upload) the pool is silent for the
    // whole 20+ s the plate waits for a key (spin219b, 2026-09-04). So the phase cannot be
    // keyed on nodes staying "live"; it is keyed on two one-shot events, both with an
    // identity of their own and both seen identically in six runs at 4:3 and 21:9 (boot43a/
    // b/c, load43g, spin219a/b):
    //
    //   plate up   - two channel-1 nodes of the HUD owner's script, +0x105F0 (opcode 0x13,
    //                operand 1) and +0x10710 (opcode 0x13, operand 0), appended once when the
    //                skull fades in; the "Load complete" plates use +0x10F0 / +0x28D0, the
    //                logos +0x300, and nothing else in the boot path runs channel 1 without
    //                a strcode. Script offsets are constant across boots.
    //   dismissed  - the continue press: the first appends after the silence are two new
    //                channels of the HUD owner, 38 with strcode 0071FC3E and 37 with
    //                0008F2D7 (opcode 0x0D), ~0.6 s after the key and before the world
    //                camera is rebuilt. (Only the rusted-barrel save has been measured; if
    //                another save's script names different channels there, the press still
    //                ends the phase by the camera rung below, a few seconds later.)
    //
    // The user's directive (2026-09-04): the bands come off at the press, not when the
    // world camera is back - a camera rung kept them up through the fade and the camera's
    // snap to the player, and at 21:9 the load's brief camera flicker ended the phase
    // before the skull was even up (spin219a). The world camera only serves the fallbacks
    // now: armed, a long stretch of it alone - no cutscene, no tabled screen - ends the
    // phase, so a path with no plate (NEW GAME) can never keep the bands up in gameplay;
    // plate-up, a short stretch does, since the camera is idle while the plate waits. The
    // title re-arms the phase (quit to title).
    enum class BootStage : int { Armed, PlateUp, Over };
    std::atomic<int> g_BootStage { static_cast<int>(BootStage::Armed) };
    std::atomic<bool> g_TitleSeen { false };
    std::atomic<bool> g_PlateUpEvent { false };           // the skull plate's show nodes were appended
    std::atomic<bool> g_PlateDismissEvent { false };      // the continue press's channels were appended
    std::atomic<ULONGLONG> g_PlateSeenTick { 0 };        // last append of a skull plate node
    std::atomic<ULONGLONG> g_GameplayBuiltTick { 0 };    // last time the gameplay camera was built
    std::atomic<uint32_t> g_GameplayBuilds { 0 };
    ULONGLONG g_BootAloneSince = 0;                       // start of the current camera-alone stretch
    constexpr ULONGLONG kBootAloneLimitMs = 15000;
    constexpr ULONGLONG kBootPlateCameraLimitMs = 3000;
    constexpr uintptr_t kSkullPlateElems[] = { 0x105F0, 0x10710 };
    constexpr uint32_t kPlateDismissStrcodes[] = { 0x0071FC3E, 0x0008F2D7 };

    bool SkullPlateNode(const AppendIdentity& id, bool hudOwner)
    {
        if (!hudOwner || id.channel != 1 || !id.hasElem || id.strcode) { return false; }
        for (const uintptr_t elem : kSkullPlateElems)
        {
            if (id.elem == elem) { return true; }
        }
        return false;
    }

    bool PlateDismissNode(const AppendIdentity& id, bool hudOwner)
    {
        if (!hudOwner || !id.strcode) { return false; }
        for (const uint32_t strcode : kPlateDismissStrcodes)
        {
            if (id.strcode == strcode) { return true; }
        }
        return false;
    }

    void PlateNote(const AppendIdentity& id, bool hudOwner)
    {
        if (SkullPlateNode(id, hudOwner))
        {
            g_PlateSeenTick.store(GetTickCount64(), std::memory_order_relaxed);
            g_PlateUpEvent.store(true, std::memory_order_relaxed);
        }
        else if (PlateDismissNode(id, hudOwner))
        {
            g_PlateDismissEvent.store(true, std::memory_order_relaxed);
        }
    }

    bool GameplayCameraLive()
    {
        return GetTickCount64() - g_GameplayBuiltTick.load(std::memory_order_relaxed) <= 150;
    }

    bool CutsceneCameraLive();

    // The press is followed by the level's intro camera - a scripted pan that snaps to the
    // player after ~4 s and runs through the cinematic route, so the cutscene rung kept the
    // bands up for exactly the stretch the user called awkward (spin219c, 2026-09-04). The
    // grace mutes that rung from the dismissal until the first present the cinematic camera
    // is not built, so a real cutscene that follows the intro still gets its bands; a save
    // that opens straight into a cutscene gives that scene's opening up to the cap.
    ULONGLONG g_PostLoadGraceUntil = 0;
    bool g_PostLoadGraceSawCamera = false;   // the intro camera has started (it may lag the dismissal)
    constexpr ULONGLONG kPostLoadGraceMs = 10000;

    bool PostLoadGrace()
    {
        if (g_PostLoadGraceUntil == 0) { return false; }
        const bool cinematic = CutsceneCameraLive();
        if (cinematic) { g_PostLoadGraceSawCamera = true; }
        if (GetTickCount64() > g_PostLoadGraceUntil || (g_PostLoadGraceSawCamera && !cinematic))
        {
            g_PostLoadGraceUntil = 0;
            spdlog::info("MGS4: letterbox: post-load grace over ({}); cutscene bands follow the camera.",
                g_PostLoadGraceSawCamera ? "intro camera done" : "no intro camera");
            return false;
        }
        return true;
    }

    // Called per present under the band mutex; the ladder's transitions live here.
    bool BootPhaseLive()
    {
        const auto stage = static_cast<BootStage>(g_BootStage.load(std::memory_order_relaxed));
        if (stage == BootStage::Over) { return false; }
        const ULONGLONG now = GetTickCount64();
        const bool cameraAlone = GameplayCameraLive() && !CutsceneCameraLive() && !ScreenLive();
        if (stage == BootStage::Armed)
        {
            g_PlateDismissEvent.store(false, std::memory_order_relaxed);
            if (g_PlateUpEvent.exchange(false, std::memory_order_relaxed))
            {
                g_BootAloneSince = 0;
                g_BootStage.store(static_cast<int>(BootStage::PlateUp), std::memory_order_relaxed);
                spdlog::info("MGS4: letterbox: boot phase: loaded-save plate up.");
                return true;
            }
        }
        else if (g_PlateDismissEvent.exchange(false, std::memory_order_relaxed))
        {
            g_BootAloneSince = 0;
            g_PostLoadGraceUntil = now + kPostLoadGraceMs;
            g_PostLoadGraceSawCamera = false;
            g_BootStage.store(static_cast<int>(BootStage::Over), std::memory_order_relaxed);
            spdlog::info("MGS4: letterbox: boot phase over (loaded-save plate dismissed); bands follow the gate.");
            return false;
        }
        // Fallback: the world camera alone, for 15 s while armed, 3 s once the plate is up.
        if (!cameraAlone)
        {
            g_BootAloneSince = 0;
            return true;
        }
        const ULONGLONG limit = stage == BootStage::Armed ? kBootAloneLimitMs : kBootPlateCameraLimitMs;
        if (g_BootAloneSince == 0) { g_BootAloneSince = now; }
        else if (now - g_BootAloneSince > limit)
        {
            g_BootAloneSince = 0;
            g_BootStage.store(static_cast<int>(BootStage::Over), std::memory_order_relaxed);
            spdlog::info("MGS4: letterbox: boot phase over (world camera alone for {} s, {}); bands follow the gate.",
                limit / 1000, stage == BootStage::Armed ? "no plate seen" : "plate up, no dismissal seen");
            return false;
        }
        return true;
    }

    void RearmBootPhase()
    {
        int over = static_cast<int>(BootStage::Over);
        if (g_BootStage.compare_exchange_strong(over, static_cast<int>(BootStage::Armed)))
        {
            spdlog::info("MGS4: letterbox: title seen; boot phase re-armed.");
        }
    }

    // Lab: "screenlog N" logs the next N distinct (owner, channel, strcode) identities the
    // gate reads, tabled or not, so a screen can be censused without a sites dump - and so
    // a tabled element that fails to fire shows what the gate saw instead.
    std::atomic<int> g_ScreenLogBudget { 0 };
    struct ScreenLogSeen { uintptr_t owner; int channel; uint32_t strcode; uint8_t opcode; uint64_t operand; };
    ScreenLogSeen g_ScreenLogSeen[512] {};
    int g_ScreenLogCount = 0;
    std::atomic<bool> g_ScreenLogAll { false };   // "screenlog N all": HUD-owner nodes without a strcode too

#if MGS4E_LAB_BUILD
    void ScreenLogAppend(const AppendIdentity& id)
    {
        if (g_ScreenLogBudget.load(std::memory_order_relaxed) <= 0) { return; }
        // Nodes without a strcode are logged for the HUD owner only (with their opcode and
        // operand, the identity they do have); the main menu's ~50 heap owners without one
        // exhausted a 60-entry budget before the save list was reached (load_43).
        const bool hud = id.owner == reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kRvaHudOwner;
        if (!id.strcode && !(hud && g_ScreenLogAll)) { return; }
        for (int i = 0; i < g_ScreenLogCount; i++)
        {
            const auto& s = g_ScreenLogSeen[i];
            if (s.owner == id.owner && s.channel == id.channel && s.strcode == id.strcode
                && s.opcode == id.opcode && s.operand == id.operand) { return; }
        }
        if (g_ScreenLogCount >= static_cast<int>(std::size(g_ScreenLogSeen)))
        {
            // The seen table is full: stop rather than log repeats (load43f logged one node
            // 40 times a second once it filled).
            g_ScreenLogBudget.store(0);
            spdlog::info("MGS4: letterbox: screenlog seen table full; log stopped.");
            return;
        }
        g_ScreenLogSeen[g_ScreenLogCount++] = { id.owner, id.channel, id.strcode, id.opcode, id.operand };
        g_ScreenLogBudget.fetch_sub(1);
        spdlog::info("MGS4: letterbox: screenlog owner {:X} ch {} elem {:X} strcode {:08X} op {:02X} operand {:016X}{}",
            id.owner, id.channel, id.hasElem ? id.elem : 0, id.strcode, id.opcode, id.operand,
            id.hasElem ? "" : " (no element frame)");
    }
#endif

    // The loading spinner (the ring in the top-right corner of the loading plates) is not a
    // layout-script widget: it has no owner, no channel and no strcode, and its record is
    // not a 96-byte anchor. It is a 48-byte record appended twice per frame from +B5830 and
    // +B58E7 (via +B5FC2 / +B5F98 under +B4FB3), with the frame-1 rsi pointing at one static
    // object of the module - its identity - and floats 4..7 holding its rectangle as a
    // fraction of the *window*: x 0.9068, y 0.8815, w 0.05, h 0.0889, a 64x64 px sprite in
    // 1280x720 terms (spin219b sites dump, 2026-09-04). Fractions of the window are why it
    // stretches with it: at 3840x1620 that came out 183x132 px and sat at the window's
    // corner, outside the 16:9 frame the rest of the interface keeps (user's read of
    // spin219a; the user asked for it scaled and placed inside that frame). The treatment
    // maps the rectangle into the centred 16:9 frame along the axis with spare room: x' =
    // (1 - 1/f)/2 + x/f, w' = w/f on a wide output, the same in y on a tall one - the ring
    // is round again and lands where a 16:9 output puts it.
    constexpr uintptr_t kRvaLoadIconObject = 0x23CF7E88;
    constexpr uint32_t kLoadIconRecordSize = 48;
    std::atomic<uint32_t> g_LoadIconLogged { 0 };

    void LoadIconAppend(SafetyHookContext& ctx)
    {
        CONTEXT c;
        BuildAppendContext(ctx, c);
        if (!UnwindOnce(c) || c.Rbp != 0 || c.Rsi != reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kRvaLoadIconObject)
        {
            return;
        }
        auto* src = reinterpret_cast<uint8_t*>(ctx.rdx);
        float rect[4] {};
        std::memcpy(rect, src + 16, sizeof(rect));
        // The rectangle as measured, or nothing: a record of this object with another
        // shape is not the sprite this was written for.
        if (rect[0] < 0.0f || rect[0] > 1.0f || rect[1] < 0.0f || rect[1] > 1.0f
            || rect[2] <= 0.0f || rect[2] > 0.5f || rect[3] <= 0.0f || rect[3] > 0.5f)
        {
            return;
        }
        const float factorX = HorizontalSpreadFactor();
        const float factorY = VerticalSpreadFactor();
        const float before[4] = { rect[0], rect[1], rect[2], rect[3] };
        rect[0] = (1.0f - 1.0f / factorX) * 0.5f + rect[0] / factorX;
        rect[2] /= factorX;
        rect[1] = (1.0f - 1.0f / factorY) * 0.5f + rect[1] / factorY;
        rect[3] /= factorY;
        std::memcpy(src + 16, rect, sizeof(rect));
        if (g_LoadIconLogged.fetch_add(1) == 0)
        {
            spdlog::info("MGS4: UI Aspect: loading spinner rect ({:.4f},{:.4f} {:.4f}x{:.4f}) -> ({:.4f},{:.4f} {:.4f}x{:.4f}) (factors {:.4f}/{:.4f}).",
                before[0], before[1], before[2], before[3], rect[0], rect[1], rect[2], rect[3], factorX, factorY);
        }
    }

    void ShiftAppend(SafetyHookContext& ctx, bool spread, bool bias, bool screens, bool icon)
    {
        if ((ctx.r8 & 0xFFFF) == kLoadIconRecordSize)
        {
            if (icon)
            {
                LoadIconAppend(ctx);
            }
            return;
        }
        if ((ctx.r8 & 0xFFFF) != kAnchorBlockSize)
        {
            return;
        }
        auto* src = reinterpret_cast<uint8_t*>(ctx.rdx);
        if (!RelaxedAnchorBlock(src))
        {
            return;
        }

        const AppendIdentity id = IdentifyAppend(ctx, spread || screens);
        if (id.channel < 0)
        {
            return;
        }
        if (screens)
        {
            const bool listed = NoteScreenElement(id.strcode);
            const bool hud = id.owner == reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kRvaHudOwner;
            ScreenFrameNote(ctx.rcx, listed);
            PlateNote(id, hud);
            if (id.strcode == 0x00042FAA)
            {
                g_TitleSeen.store(true, std::memory_order_relaxed);
                RearmBootPhase();
            }
#if MGS4E_LAB_BUILD
            ScreenLogAppend(id);
#endif
        }
        if (!spread && !bias)
        {
            return;
        }
        if (NewFrameAt(ctx.rcx))
        {
            g_OrdCount = 0;
            std::memcpy(g_ChannelLast, g_ChannelNow, sizeof(g_ChannelLast));
            std::memset(g_ChannelNow, 0, sizeof(g_ChannelNow));
        }

        // Two independently switchable treatments share the walk: the world-tracked groups
        // (bAspectWorldTracking) and the corner/selector spread (bAspectPoolSpread). Each
        // names only its own channels, so ChannelDx carries a group only when its switch is
        // on - a centered HUD with world tracking moves the Solid Eye labels and nothing else.
        // Every treatment is a pair: the horizontal delta is live on outputs wider than
        // 16:9, the vertical one on outputs taller, and the inactive axis's shift is 0.0 so
        // the pair reduces to the one number the output needs.
        float dx = 0.0f;
        float dy = 0.0f;
        if (spread && id.owner == reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kRvaHudOwner)
        {
            const bool corners = bAspectPoolSpread;
            ChannelTreatment& now = g_ChannelNow[id.channel];
            if (const WorldElement* world = bAspectWorldTracking ? WorldElementEntry(id.strcode) : nullptr)
            {
                const int worldMode = g_WorldMode.load(std::memory_order_relaxed);
                if (worldMode != 0 && (worldMode == 2 || !now.named))
                {
                    float v8[2] {};
                    std::memcpy(v8, src + kAnchorBlockV8, sizeof(v8));
                    now = { v8[0] * (HorizontalSpreadFactor() - 1.0f), v8[1] * (VerticalSpreadFactor() - 1.0f), true };
                    const uint64_t logBit = 1ull << (world - kWorldElements);
                    if ((g_WorldElementsLogged.fetch_or(logBit) & logBit) == 0)
                    {
                        spdlog::info("MGS4: UI Aspect: world element {:08X} ({}) on channel 0x{:02X}: reference element +{:X}, x {:.4f} -> {:.4f}, y {:.4f} -> {:.4f}.",
                            world->strcode, world->what, id.channel, id.elem, v8[0], v8[0] + now.dx, v8[1], v8[1] + now.dy);
                    }
                }
            }
            else if (const HudIdentity* identity = corners ? HudIdentityEntry(id.strcode) : nullptr)
            {
                now = { static_cast<float>(identity->side) * SideShift(), static_cast<float>(identity->sideY) * SideShiftY(), true };
                const uint64_t logBit = 1ull << (identity - kHudIdentities);
                // A widget with no side on the active axis (the top-centre banner at 21:9) is
                // carried as a zero so its channel's satellites stay put too; nothing to log.
                if ((now.dx != 0.0f || now.dy != 0.0f) && (g_ShiftedIdentitiesLogged.fetch_or(logBit) & logBit) == 0)
                {
                    spdlog::info("MGS4: UI Aspect: element {:08X} ({}) on channel 0x{:02X} anchored {} by {:.4f}.",
                        identity->strcode, identity->what, id.channel, ShiftEdgeName(identity->side, identity->sideY), ActiveShift());
                }
            }

            // The selector overlay is the one case keyed by element offset: its script holds
            // both wheels' backdrops, plates and text under a single strcode, and each needs
            // its own treatment. None of them names the channel - the text stays centred.
            const ElementSide* element = (corners && id.hasElem) ? HudElementEntry(id.strcode, id.elem) : nullptr;
            if (element)
            {
                dx += static_cast<float>(element->side) * SideShift();
                dy += static_cast<float>(element->sideY) * SideShiftY();
                const uint64_t bit = 1ull << (element - kHudElements);
                if ((g_ShiftedElementsLogged.fetch_or(bit) & bit) == 0)
                {
                    spdlog::info("MGS4: UI Aspect: element {:08X}+{:X} ({}) on channel 0x{:02X} anchored {} by {:.4f}.",
                        element->strcode, id.elem, element->what, id.channel, ShiftEdgeName(element->side, element->sideY), ActiveShift());
                }
            }
            else if (const ElementStretch* stretch = (corners && id.hasElem) ? HudStretchEntry(id.strcode, id.elem) : nullptr)
            {
                // Stretched about the screen centre: basis and origin scaled together along
                // the axis with spare room, so the plate's 16:9 span becomes the full output
                // span - sideways on a wide output, up and down on a tall one.
                const float factorX = HorizontalSpreadFactor();
                const float factorY = VerticalSpreadFactor();
                float basisX = 0.0f, basisY = 0.0f, originX = 0.0f, originY = 0.0f;
                std::memcpy(&basisX, src + kAnchorBlockV6, sizeof(basisX));
                std::memcpy(&basisY, src + kAnchorBlockV7 + 4, sizeof(basisY));
                std::memcpy(&originX, src + kAnchorBlockV8, sizeof(originX));
                std::memcpy(&originY, src + kAnchorBlockV8 + 4, sizeof(originY));
                basisX *= factorX;
                originX *= factorX;
                basisY *= factorY;
                originY *= factorY;
                std::memcpy(src + kAnchorBlockV6, &basisX, sizeof(basisX));
                std::memcpy(src + kAnchorBlockV7 + 4, &basisY, sizeof(basisY));
                std::memcpy(src + kAnchorBlockV8, &originX, sizeof(originX));
                std::memcpy(src + kAnchorBlockV8 + 4, &originY, sizeof(originY));
                const uint64_t bit = 1ull << (stretch - kHudStretched);
                if ((g_StretchedElementsLogged.fetch_or(bit) & bit) == 0)
                {
                    spdlog::info("MGS4: UI Aspect: element {:08X}+{:X} ({}) on channel 0x{:02X} stretched by {:.4f}.",
                        stretch->strcode, id.elem, stretch->what, id.channel, factorX != 1.0f ? factorX : factorY);
                }
            }
            else if (const ChannelTreatment* carry = ChannelCarry(id.channel))
            {
                dx += carry->dx;
                dy += carry->dy;
            }
        }
        if (bias)
        {
            dx += g_ChannelBias[id.channel];
            dy += g_ChannelBiasY[id.channel];
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
        if (dx != 0.0f || dy != 0.0f)
        {
            float v8[2] {};
            std::memcpy(v8, src + kAnchorBlockV8, sizeof(v8));
            v8[0] += dx;
            v8[1] += dy;
            std::memcpy(src + kAnchorBlockV8, v8, sizeof(v8));
        }
    }

    void PoolAppendProbe(SafetyHookContext& ctx)
    {
#if MGS4E_LAB_BUILD
        PerfScope perf(g_Perf.appendCalls, g_Perf.appendTicks, &g_Perf.appendMax);
#endif
        const bool sites = g_SiteDumpFrames.load(std::memory_order_relaxed) > 0;
        const bool bias = g_ChannelBiasActive.load(std::memory_order_relaxed);
        const bool shifted = SideShift() != 0.0f || SideShiftY() != 0.0f;   // false at 16:9: strict no-op
        const bool spread = (bAspectPoolSpread || bAspectWorldTracking) && shifted;
        // The masking reads the element identity too, whenever the output has bands to
        // draw: the menu gate and the boot ladder for the menu masking, the ladder alone
        // (its dismissal starts the post-load grace) for the cutscene masking.
        const bool screens = (bMenuMasking || bCutsceneMasking) && shifted;
        // The loading spinner is mapped into the 16:9 frame whenever the HUD is: it is the
        // one HUD element drawn outside the pool's anchor records (AR-011).
        const bool icon = bFixUiAspect && shifted;
        if (!sites && !bias && !spread && !screens && !icon)
        {
            return;
        }

        if (bias || spread || screens || icon)
        {
            ShiftAppend(ctx, spread, bias, screens, icon);
        }

        if (!sites)
        {
            return;
        }

        AppendEvent ev {};
        ev.dst = ctx.rcx;
        ev.size = static_cast<uint32_t>(ctx.r8 & 0xFFFF);   // movzx r8d, si at +68A92B
        std::memcpy(ev.head, reinterpret_cast<const void*>(ctx.rdx), (std::min<size_t>)(ev.size, sizeof(ev.head)));

        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
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
        auto* at = reinterpret_cast<uint8_t*>(mgs4e::game::Module()) + kRvaPoolAppendCall;
        if (std::memcmp(at, kExpected, sizeof(kExpected)) != 0)
        {
            spdlog::error("MGS4: pool append attribution: +{:X} is not the expected call - not hooking.",
                kRvaPoolAppendCall);
            return;
        }
        PoolAppend_hook = safetyhook::create_mid(at, PoolAppendProbe);
        MGS4E_LOG_HOOK(PoolAppend_hook, "MGS4: pool append attribution")
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
        // Appends that landed outside the pool being uploaded are listed the same way, with
        // their destination: the loading screens append into neither of the ping-pong pools
        // (25 events, 0 inside; spin219a 05:24:04, 2026-09-04), so whatever draws there -
        // the loading spinner is the suspect - has a third buffer.
        int others = 0;
        for (const auto& e : events)
        {
            const bool outside = e.dst < source || e.dst >= source + bytes;
            if ((!outside && e.size == 96) || others >= 400)
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
            spdlog::info("MGS4: sites: rec {}/{} owner={:X} ch={:X} f=[{}] hex={} {}",
                outside ? std::format("out@{:X}", e.dst) : std::format("+{:05X}", e.dst - source),
                e.size, e.regs[1][1], e.regs[1][2], floats, hex, chain);
        }
        spdlog::info("MGS4: sites: {} non-anchor / outside records listed.", others);
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
        const auto moduleBase = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
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
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
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
            reinterpret_cast<uint8_t*>(mgs4e::game::Module()) + kRvaProducerWrapper, ProducerWrapperProbe);
        MGS4E_LOG_HOOK(ProducerWrapper_hook, "MGS4: producer probe (wrapper)")
        ProducerOuter_hook = safetyhook::create_mid(
            reinterpret_cast<uint8_t*>(mgs4e::game::Module()) + kRvaProducerOuter, ProducerOuterProbe);
        MGS4E_LOG_HOOK(ProducerOuter_hook, "MGS4: producer probe (outer)")
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
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        spdlog::info("MGS4: UI channel: sel={:X} size={:X} ret=+{:X}",
            static_cast<uint32_t>(ctx.rcx), static_cast<uint32_t>(ctx.rdx),
            (returnAddress >= base) ? returnAddress - base : returnAddress);
    }

    void InstallChannelCensus()
    {
        ChannelEmit_hook = safetyhook::create_mid(
            reinterpret_cast<uint8_t*>(mgs4e::game::Module()) + kRvaChannelEmitCall, ChannelEmitProbe);
        MGS4E_LOG_HOOK(ChannelEmit_hook, "MGS4: UI channel census")

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
            if (uint8_t* info = mgs4e::mem::FindPattern(mgs4e::game::Module(), kArchiveFileInfoSignature, "MGS4: archive file info"))
            {
                g_ArchiveFileInfo = reinterpret_cast<ArchiveFileInfoFn>(info);
            }

            if (uint8_t* at = mgs4e::mem::FindPattern(mgs4e::game::Module(), kSlotResourceLoadSignature, "MGS4: slot resource logging"))
            {
                SlotResourceLoad_hook = safetyhook::create_inline(at,
                    reinterpret_cast<void*>(&LoggedSlotResourceLoad));
                MGS4E_LOG_HOOK(SlotResourceLoad_hook, "MGS4: slot resource logging")
            }
        }
        if (uint8_t* at = mgs4e::mem::FindPattern(mgs4e::game::Module(), kArchiveReadSignature, "MGS4: LA2 logging"))
        {
            ArchiveRead_hook = safetyhook::create_inline(at, reinterpret_cast<void*>(&LoggedArchiveRead));
            MGS4E_LOG_HOOK(ArchiveRead_hook, "MGS4: LA2 logging")
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
            const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
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
        if (uint8_t* at = mgs4e::mem::FindPattern(mgs4e::game::Module(), kRectEmitterSignature, "MGS4: rect emitter"))
        {
            RectEmitter_hook = safetyhook::create_inline(at, reinterpret_cast<void*>(&LoggedRectEmit));
            MGS4E_LOG_HOOK(RectEmitter_hook, "MGS4: rect emitter")
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
#if MGS4E_LAB_BUILD
    constexpr const char* kLiveCommandFile = "C:/mgspf_tools/live_anchor.txt";
#endif

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
        const auto moduleBase = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
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

    // Arm one emit-attribution dump: snapshot the registry for the append probe's stack
    // scan, drop any stale events, then arm one pool dump's worth of frames. The caller
    // holds g_ObjectsMutex.
    void ArmSiteDump(const char* who)
    {
        {
            std::lock_guard appendLock(g_AppendMutex);
            g_SiteObjects = g_Objects;
            g_AppendEvents.clear();
            g_AppendEvents.reserve(4096);
        }
        g_SiteDumpFrames.store(2);
        spdlog::info("MGS4: {}: emit attribution armed ({} objects).", who, g_SiteObjects.size());
    }

    // ---- letterbox / pillarbox (AR-010) ----------------------------------------------------
    //
    // Cutscenes, the codec, the pause menu and the title are 16:9 compositions. Layer 1
    // already centres their UI; the picture behind them is not centred, it is re-framed: the
    // engine holds the horizontal angle at every output ratio and lets the vertical float, so
    // a 21:9 window loses the top and bottom of every cinematic frame (Vert-) and a 4:3 window
    // gains sky and floor (Vert+). Measured 2026-09-04: s01a10l_D2 Snake close-up at equal
    // width, 16:9 against 21:9; the gameplay trio fov_169/219/43.
    //
    // Two pieces put the 16:9 picture back:
    //   - Camera. The cinematic camera builder takes a projection scale; dividing it by the
    //     horizontal spread factor is a uniform zoom-out - m00 and m11 shrink together, the
    //     matrix keeps the window's ratio - and because it lands at the builder's input the
    //     engine rebuilds its frustum planes to match, so nothing pops at the recovered top
    //     and bottom edges. At 4:3 the stock frame already contains the 16:9 picture, so the
    //     camera is left alone there (the factor is 1.0).
    //   - Bands. Black rects cleared onto the back buffer right before Present, over the
    //     region outside the centred 16:9 rect. At 21:9 they cover the extra world the zoom
    //     reveals at the sides; at 4:3 the extra sky and floor - and the HUD widgets the
    //     title screen draws there.
    //
    // The sites are mgs4Ultra120's (github.com/drbermejor/mgs4Ultra120; same executable
    // build, TimeDateStamp 6A8CFC47), each prologue-checked in-process before hooking. The
    // cutscene signal is structural rather than a state flag: +0x652E00 owns the in-engine
    // cinematic camera and reaches the builder through the return at +0x0B9BA0 only while a
    // cinematic camera is live; a thread-local depth counter around the owner tags exactly
    // those builder calls. The final rebuild at +0x0EB0EB runs outside the owner's scope and
    // is recognised the way Ultra120 does it: same camera object, same input scale, within a
    // quarter second of the tagged call.
    constexpr uintptr_t kRvaCameraBuilder = 0x0B9BB0;
    constexpr uintptr_t kRvaCinematicOwner = 0x652E00;
    constexpr uintptr_t kRvaProjectionSetter = 0x0E3410;
    constexpr uintptr_t kRvaCameraGameplayReturn = 0x0BA3A3;
    constexpr uintptr_t kRvaCameraCinematicSourceReturn = 0x0B9BA0;
    constexpr uintptr_t kRvaCameraCinematicFinalReturn = 0x0EB0EB;

    SafetyHookInline CameraBuilder_hook {};
    SafetyHookInline CinematicOwner_hook {};
    SafetyHookInline ProjectionSetter_hook {};

    thread_local unsigned g_CinematicOwnerDepth = 0;
    thread_local void* g_CinematicCamera = nullptr;
    thread_local ULONGLONG g_CinematicCameraTick = 0;
    thread_local float g_CinematicInputScale = 0.0f;
    thread_local void* g_GameplayCamera = nullptr;
    thread_local ULONGLONG g_GameplayCameraTick = 0;
    thread_local float g_GameplayInputScale = 0.0f;

    // Cross-thread state for the band gate and the live lab.
    std::atomic<ULONGLONG> g_CinematicBuiltTick { 0 };   // last time a cinematic camera was built
    std::atomic<int> g_CutsceneZoom { 0 };               // live "cutzoom": 0 off, 1 on; seeded from bCutsceneFovCompensation
    std::atomic<float> g_CutsceneDivisor { 0.0f };       // live "cutdiv": explicit divisor, 0 = the factor
    std::atomic<int> g_BandMode { 0 };                   // live "box": 0 cutscene-gated, 1 forced on, 2 forced off
    std::atomic<int> g_CameraLogBudget { 0 };            // live "camlog N": builder calls to log
    std::atomic<int> g_ProjectionLogBudget { 0 };        // live "projlog N": setter calls to log
    std::atomic<uint32_t> g_CameraPatches { 0 };

    bool CutsceneCameraLive()
    {
        return GetTickCount64() - g_CinematicBuiltTick.load() <= 150;
    }

    // The builder: void(camera, source, projection_scale, p4, p5, aspect_scale). Only the
    // scale is touched: divided by the spread factor on the tagged cinematic routes (the
    // cutscene FOV compensation) and by the FOV adjustment on the gameplay route at
    // +0x0BA3A3; everything else passes through untouched. The original does all the work -
    // nothing in the camera object is rewritten afterwards (Ultra120 measured the double
    // transform that produces).
    void __fastcall BuildCamera(void* camera, const void* source, float projectionScale,
        float p4, float p5, float aspectScale)
    {
        const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        const uintptr_t rva = ret >= base ? ret - base : ret;
        const ULONGLONG now = GetTickCount64();

        const bool cinematicSource = g_CinematicOwnerDepth != 0 && rva == kRvaCameraCinematicSourceReturn;
        const float tolerance = std::max(0.001f, std::fabs(g_CinematicInputScale) * 0.001f);
        const bool cinematicFinal = rva == kRvaCameraCinematicFinalReturn
            && camera == g_CinematicCamera
            && now - g_CinematicCameraTick <= 250
            && std::fabs(projectionScale - g_CinematicInputScale) <= tolerance;

        const bool usable = std::isfinite(projectionScale) && projectionScale > 0.0f;
        float scale = projectionScale;
        const bool gameplay = rva == kRvaCameraGameplayReturn;
        // The gameplay camera is rebuilt several times per frame after the tagged call
        // (+0x0B9BA0 outside the owner, +0x0EB0EB, +0x0EB191, +0x0B8A43, +0x0B8B4F; fov100,
        // 2026-09-04), each with the same input scale; the last write is what shows, so the
        // adjustment follows the camera object the way the cutscene's final rebuild does.
        const float gameplayTolerance = std::max(0.001f, std::fabs(g_GameplayInputScale) * 0.001f);
        const bool gameplayRebuild = !gameplay && !cinematicSource && !cinematicFinal
            && camera == g_GameplayCamera
            && now - g_GameplayCameraTick <= 250
            && std::fabs(projectionScale - g_GameplayInputScale) <= gameplayTolerance;
        if (gameplay)
        {
            g_GameplayBuiltTick.store(now, std::memory_order_relaxed);
            g_GameplayBuilds.fetch_add(1, std::memory_order_relaxed);
            g_GameplayCamera = camera;
            g_GameplayCameraTick = now;
            g_GameplayInputScale = projectionScale;
        }
        if (gameplay || gameplayRebuild)
        {
            const float fov = fFovAdjustment;
            if (usable && std::isfinite(fov) && fov > 0.0f && fov != 1.0f)
            {
                scale = projectionScale / fov;
            }
        }
        if (cinematicSource || cinematicFinal)
        {
            g_CinematicBuiltTick.store(now);
            if (cinematicSource)
            {
                g_CinematicCamera = camera;
                g_CinematicCameraTick = now;
                g_CinematicInputScale = projectionScale;
            }
            if (g_CutsceneZoom.load() != 0)
            {
                const float divisor = g_CutsceneDivisor.load() > 0.0f ? g_CutsceneDivisor.load() : HorizontalSpreadFactor();
                if (usable && divisor > 0.0f && divisor != 1.0f)
                {
                    scale = projectionScale / divisor;
                    g_CameraPatches.fetch_add(1);
                }
            }
        }

        if (g_CameraLogBudget.load() > 0 && g_CameraLogBudget.fetch_sub(1) > 0)
        {
            spdlog::info("MGS4: letterbox: camera build from +{:X} ({}) cam={:X} scale {:.5f} -> {:.5f} p4={:.4f} p5={:.4f} aspect={:.4f} depth={}",
                rva,
                gameplay ? "gameplay" : gameplayRebuild ? "gameplay-rebuild" : cinematicSource ? "cinematic" : cinematicFinal ? "cinematic-final" : "other",
                reinterpret_cast<uintptr_t>(camera), projectionScale, scale, p4, p5, aspectScale, g_CinematicOwnerDepth);
        }

        CameraBuilder_hook.call<void>(camera, source, scale, p4, p5, aspectScale);
    }

    // The owner takes one argument; the other three registers are forwarded regardless so
    // a wider signature would still survive.
    void __fastcall UpdateCinematicCamera(void* a, void* b, void* c, void* d)
    {
        ++g_CinematicOwnerDepth;
        CinematicOwner_hook.call<void>(a, b, c, d);
        --g_CinematicOwnerDepth;
    }

    // Passthrough with a log budget: the common projection setter sees every matrix the
    // renderer is given, so "projlog" answers whether the zoom reached the render side
    // as a uniform scale (m11/m00 unchanged) and nothing applied it twice.
    void __fastcall SetProjection(const float* matrix)
    {
        if (matrix && g_ProjectionLogBudget.load() > 0 && g_ProjectionLogBudget.fetch_sub(1) > 0)
        {
            spdlog::info("MGS4: letterbox: projection m00={:.5f} m11={:.5f} m11/m00={:.4f} m[11]={:+.3f} m[10]={:.5f} m[14]={:.4f} cutscene={}",
                matrix[0], matrix[5], matrix[0] != 0.0f ? matrix[5] / matrix[0] : 0.0f, matrix[11], matrix[10], matrix[14],
                CutsceneCameraLive() ? "yes" : "no");
        }
        ProjectionSetter_hook.call<void>(matrix);
    }

    // ---- the bands: a clear on the back buffer before Present -----------------------------
    //
    // The game's swap chain is reached through the DXGI factory exports (hooked the way
    // render_pipeline.cpp hooks D3D12CreateDevice), which hands over both the swap chain and the command
    // queue it presents from. Each Present records one small command list on that queue:
    // transition the current back buffer, clear the band rects, transition back. Same queue,
    // so it is ordered after the game's frame and before the present.
    SafetyHookInline CreateDXGIFactory_hook {};
    SafetyHookInline CreateDXGIFactory1_hook {};
    SafetyHookInline CreateDXGIFactory2_hook {};
    SafetyHookInline CreateSwapChain_hook {};
    SafetyHookInline CreateSwapChainForHwnd_hook {};
    SafetyHookInline Present_hook {};
    SafetyHookInline Present1_hook {};
    SafetyHookInline ResizeBuffers_hook {};
    SafetyHookInline ResizeBuffers1_hook {};

    // IDXGIFactory / IDXGIFactory2 / IDXGISwapChain / IDXGISwapChain1 / IDXGISwapChain3 vtable slots.
    constexpr size_t kFactoryCreateSwapChainSlot = 10;
    constexpr size_t kFactory2CreateSwapChainForHwndSlot = 15;
    constexpr size_t kSwapChainPresentSlot = 8;
    constexpr size_t kSwapChainResizeBuffersSlot = 13;
    constexpr size_t kSwapChain1Present1Slot = 22;
    constexpr size_t kSwapChain3ResizeBuffers1Slot = 39;

    struct BandRenderer
    {
        IDXGISwapChain3* swapChain = nullptr;
        ID3D12Device* device = nullptr;
        ID3D12CommandQueue* queue = nullptr;
        ID3D11Device* device11 = nullptr;            // set instead of the D3D12 trio on a D3D11 chain
        ID3D11DeviceContext1* context11 = nullptr;
        ID3D12DescriptorHeap* rtvHeap = nullptr;
        std::vector<ID3D12Resource*> buffers;
        std::vector<ID3D12CommandAllocator*> allocators;
        std::vector<UINT64> fenceValues;
        ID3D12GraphicsCommandList* list = nullptr;
        ID3D12Fence* fence = nullptr;
        HANDLE fenceEvent = nullptr;
        UINT64 fenceCounter = 0;
        UINT rtvStride = 0;
        UINT width = 0;
        UINT height = 0;
        bool failed = false;
        uint32_t presents = 0;
        uint32_t cleared = 0;
    };
    BandRenderer g_Band;
    std::mutex g_BandMutex;
    ID3D12CommandQueue* g_PresentQueue = nullptr;
    // Lab: what the last wanted present painted (boxstat), for reading a capture against it.
    UINT g_BandLastPainted[2] {};
    D3D12_RECT g_BandLastRects[2] {};
    UINT g_BandLastIndex = 0;

    template <typename T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

    // ---- fit to display (AR-014) --------------------------------------------------------------
    //
    // The engine's fullscreen is a borderless window at the desktop size, and it paints its
    // picture - render.windowSize pixels - at the origin of that swap chain, unscaled: the
    // last thing in a frame is a bind of the back buffer, a viewport of (0,0,windowSize) and
    // one full-screen triangle (D3D11 census, 2026-09-05). The launcher always sets windowSize
    // to the desktop size, so nobody sees this; the Window Aspect Ratio override is what makes
    // them differ - a 1400x1050 picture in the corner of a 3840x2160 chain, 3840x1620 in the
    // top band. So every viewport and scissor the engine sets while the back buffer is bound
    // is remapped into the largest centred rect of the chain with the window's aspect, and the
    // masking bands are drawn in that rect. Nothing upstream changes: the engine still lays
    // out, renders and reads its window size exactly as before.
    struct FitRect
    {
        bool active = false;   // false when the window size is the chain size (or unknown)
        float scale = 1.0f;
        float ox = 0.0f, oy = 0.0f;
        UINT w = 0, h = 0;     // the fitted rect
        int ww = 0, wh = 0;    // the window size it was computed for
        UINT cw = 0, ch = 0;   // the chain size it was computed for
    };
    FitRect g_Fit;   // recomputed at each present; read on the render thread

    FitRect ComputeFit(UINT chainW, UINT chainH)
    {
        FitRect f;
        f.w = chainW;
        f.h = chainH;
        f.cw = chainW;
        f.ch = chainH;
        f.ww = RenderPipeline::pWindowResX ? *RenderPipeline::pWindowResX : 0;
        f.wh = RenderPipeline::pWindowResY ? *RenderPipeline::pWindowResY : 0;
        if (f.ww <= 0 || f.wh <= 0 || chainW == 0 || chainH == 0
            || (static_cast<UINT>(f.ww) == chainW && static_cast<UINT>(f.wh) == chainH))
        {
            return f;
        }
        f.scale = (std::min)(static_cast<float>(chainW) / static_cast<float>(f.ww), static_cast<float>(chainH) / static_cast<float>(f.wh));
        f.w = static_cast<UINT>(std::lround(f.ww * f.scale));
        f.h = static_cast<UINT>(std::lround(f.wh * f.scale));
        f.ox = static_cast<float>((chainW - f.w) / 2);
        f.oy = static_cast<float>((chainH - f.h) / 2);
        f.active = true;
        return f;
    }

    // A viewport in window space (its extent no larger than the window) is moved into the
    // fitted rect. One already in chain space - a full-chain clear, say - is left alone.
    template <typename Viewport>
    bool FitViewport(Viewport& vp)
    {
        if (!g_Fit.active || vp.Width > g_Fit.ww + 1.0f || vp.Height > g_Fit.wh + 1.0f) { return false; }
        vp.TopLeftX = g_Fit.ox + vp.TopLeftX * g_Fit.scale;
        vp.TopLeftY = g_Fit.oy + vp.TopLeftY * g_Fit.scale;
        vp.Width *= g_Fit.scale;
        vp.Height *= g_Fit.scale;
        return true;
    }

    bool FitScissor(RECT& r)
    {
        if (!g_Fit.active || r.right - r.left > g_Fit.ww + 1 || r.bottom - r.top > g_Fit.wh + 1) { return false; }
        r.left = static_cast<LONG>(std::lround(g_Fit.ox + r.left * g_Fit.scale));
        r.top = static_cast<LONG>(std::lround(g_Fit.oy + r.top * g_Fit.scale));
        r.right = static_cast<LONG>(std::lround(g_Fit.ox + r.right * g_Fit.scale));
        r.bottom = static_cast<LONG>(std::lround(g_Fit.oy + r.bottom * g_Fit.scale));
        return true;
    }

    // D3D11: the immediate context. The back buffer is known by identity (pointer value from
    // GetBuffer, released at once, never dereferenced); a resize clears it.
    SafetyHookInline Fit11_OMSetRenderTargets_hook {};
    SafetyHookInline Fit11_OMSetRenderTargetsAndUAVs_hook {};
    SafetyHookInline Fit11_RSSetViewports_hook {};
    SafetyHookInline Fit11_RSSetScissorRects_hook {};
    void* g_BackBuffer11 = nullptr;
    bool g_BackBufferBound11 = false;

    bool IsBackBuffer11(ID3D11RenderTargetView* view)
    {
        if (!view || !g_BackBuffer11) { return false; }
        ID3D11Resource* res = nullptr;
        view->GetResource(&res);
        const bool back = res == g_BackBuffer11;
        if (res) { res->Release(); }
        return back;
    }

    void STDMETHODCALLTYPE Fit11_OMSetRenderTargets(ID3D11DeviceContext* self, UINT count,
        ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth)
    {
        g_BackBufferBound11 = count > 0 && views && IsBackBuffer11(views[0]);
        Fit11_OMSetRenderTargets_hook.stdcall<void>(self, count, views, depth);
    }

    void STDMETHODCALLTYPE Fit11_OMSetRenderTargetsAndUAVs(ID3D11DeviceContext* self, UINT count,
        ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth, UINT uavStart, UINT uavCount,
        ID3D11UnorderedAccessView* const* uavs, const UINT* initialCounts)
    {
        if (count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        {
            g_BackBufferBound11 = count > 0 && views && IsBackBuffer11(views[0]);
        }
        Fit11_OMSetRenderTargetsAndUAVs_hook.stdcall<void>(self, count, views, depth, uavStart, uavCount, uavs, initialCounts);
    }

    void STDMETHODCALLTYPE Fit11_RSSetViewports(ID3D11DeviceContext* self, UINT count, const D3D11_VIEWPORT* viewports)
    {
        if (g_BackBufferBound11 && g_Fit.active && count > 0 && count <= 16 && viewports)
        {
            D3D11_VIEWPORT fitted[16];
            std::memcpy(fitted, viewports, sizeof(D3D11_VIEWPORT) * count);
            for (UINT i = 0; i < count; i++) { FitViewport(fitted[i]); }
            Fit11_RSSetViewports_hook.stdcall<void>(self, count, fitted);
            return;
        }
        Fit11_RSSetViewports_hook.stdcall<void>(self, count, viewports);
    }

    void STDMETHODCALLTYPE Fit11_RSSetScissorRects(ID3D11DeviceContext* self, UINT count, const D3D11_RECT* rects)
    {
        if (g_BackBufferBound11 && g_Fit.active && count > 0 && count <= 16 && rects)
        {
            D3D11_RECT fitted[16];
            std::memcpy(fitted, rects, sizeof(D3D11_RECT) * count);
            for (UINT i = 0; i < count; i++) { FitScissor(fitted[i]); }
            Fit11_RSSetScissorRects_hook.stdcall<void>(self, count, fitted);
            return;
        }
        Fit11_RSSetScissorRects_hook.stdcall<void>(self, count, rects);
    }

    void InstallFit11(ID3D11DeviceContext* context)
    {
        static bool installed = false;
        if (installed || !context) { return; }
        installed = true;
        void** vtable = *reinterpret_cast<void***>(context);
        Fit11_OMSetRenderTargets_hook = safetyhook::create_inline(vtable[33], reinterpret_cast<void*>(Fit11_OMSetRenderTargets));
        Fit11_OMSetRenderTargetsAndUAVs_hook = safetyhook::create_inline(vtable[34], reinterpret_cast<void*>(Fit11_OMSetRenderTargetsAndUAVs));
        Fit11_RSSetViewports_hook = safetyhook::create_inline(vtable[44], reinterpret_cast<void*>(Fit11_RSSetViewports));
        Fit11_RSSetScissorRects_hook = safetyhook::create_inline(vtable[45], reinterpret_cast<void*>(Fit11_RSSetScissorRects));
        spdlog::info("MGS4: fit: D3D11 context hooks (render targets {}/{}, viewports {}, scissors {}).",
            Fit11_OMSetRenderTargets_hook ? "ok" : "FAILED", Fit11_OMSetRenderTargetsAndUAVs_hook ? "ok" : "FAILED",
            Fit11_RSSetViewports_hook ? "ok" : "FAILED", Fit11_RSSetScissorRects_hook ? "ok" : "FAILED");
    }

    // D3D12: render targets are descriptor handles, so the back buffer is known by the RTVs
    // the game creates for the chain's buffers (CreateRenderTargetView, hooked from the swap
    // chain's creation on, before the game makes them); a resize empties the table and the
    // game's re-creation refills it. Bound state is per command list.
    SafetyHookInline Fit12_CreateRenderTargetView_hook {};
    SafetyHookInline Fit12_OMSetRenderTargets_hook {};
    SafetyHookInline Fit12_RSSetViewports_hook {};
    SafetyHookInline Fit12_RSSetScissorRects_hook {};
    IDXGISwapChain* g_FitSwapChain12 = nullptr;   // identity only; the chain outlives every use
    std::mutex g_Fit12Mutex;
    std::vector<SIZE_T> g_BackBufferRtvs12;
    std::unordered_map<void*, bool> g_BoundLists12;

    bool IsSwapChainBuffer12(ID3D12Resource* res)
    {
        if (!res || !g_FitSwapChain12) { return false; }
        DXGI_SWAP_CHAIN_DESC desc {};
        if (FAILED(g_FitSwapChain12->GetDesc(&desc))) { return false; }
        for (UINT i = 0; i < desc.BufferCount; i++)
        {
            ID3D12Resource* buffer = nullptr;
            if (SUCCEEDED(g_FitSwapChain12->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&buffer))) && buffer)
            {
                const bool match = buffer == res;
                buffer->Release();
                if (match) { return true; }
            }
        }
        return false;
    }

    // Every RTV handle's resource size (AR-015, second part). Under D3D12 the engine sizes
    // its composition targets by the swap chain and its viewports by the window, so with
    // the override on it draws a window-sized picture into the top-left of a chain-sized
    // target and the final blit copies that 1:1 (census 2026-09-05: the 16:9-band pass on a
    // 1600x1200 target with a 1400x1050 window, the back-buffer blit at (0,0 1600x1200)).
    // The fit therefore applies to viewports set on chain-sized targets as well as on the
    // back buffer; the blit's own chain-sized viewport is left alone by FitViewport.
    std::unordered_map<SIZE_T, std::pair<UINT, UINT>> g_RtvSizes12;   // under g_Fit12Mutex
#if MGS4E_LAB_BUILD
    std::string RtvSize12(SIZE_T handle)
    {
        std::lock_guard lock(g_Fit12Mutex);
        const auto it = g_RtvSizes12.find(handle);
        return it == g_RtvSizes12.end() ? "?" : std::format("{}x{}", it->second.first, it->second.second);
    }
#endif

    void STDMETHODCALLTYPE Fit12_CreateRenderTargetView(ID3D12Device* self, ID3D12Resource* res,
        const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
    {
        Fit12_CreateRenderTargetView_hook.stdcall<void>(self, res, desc, dest);
        std::lock_guard lock(g_Fit12Mutex);
        std::erase(g_BackBufferRtvs12, dest.ptr);
        if (IsSwapChainBuffer12(res)) { g_BackBufferRtvs12.push_back(dest.ptr); }
        if (res)
        {
            const D3D12_RESOURCE_DESC rd = res->GetDesc();
            g_RtvSizes12[dest.ptr] = { static_cast<UINT>(rd.Width), static_cast<UINT>(rd.Height) };
        }
    }

    void STDMETHODCALLTYPE Fit12_OMSetRenderTargets(ID3D12GraphicsCommandList* self, UINT count,
        const D3D12_CPU_DESCRIPTOR_HANDLE* handles, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* depth)
    {
        {
            std::lock_guard lock(g_Fit12Mutex);
            bool bound = count > 0 && handles
                && std::find(g_BackBufferRtvs12.begin(), g_BackBufferRtvs12.end(), handles[0].ptr) != g_BackBufferRtvs12.end();
            if (!bound && count > 0 && handles && g_Fit.active)
            {
                const auto it = g_RtvSizes12.find(handles[0].ptr);
                bound = it != g_RtvSizes12.end() && it->second.first == g_Fit.cw && it->second.second == g_Fit.ch;
            }
            g_BoundLists12[self] = bound;
        }
        Fit12_OMSetRenderTargets_hook.stdcall<void>(self, count, handles, single, depth);
    }

    bool ListBound12(ID3D12GraphicsCommandList* list)
    {
        std::lock_guard lock(g_Fit12Mutex);
        const auto it = g_BoundLists12.find(list);
        return it != g_BoundLists12.end() && it->second;
    }

    // ---- AR-015: the D3D12 path's 16:9 band ----------------------------------------------
    //
    // In a window taller than 16:9 (4:3, say) the D3D12 renderer draws its whole picture -
    // the resolved scene and the interface with it - into the centred 16:9 band of a
    // window-sized target, and the final blit shows that target as is. The scene is
    // rendered with the window's projection, so the picture comes out squeezed vertically
    // and the rest of the target is never cleared (garbage in the corners). Measured
    // 2026-09-05 on a 1600x1200 window: viewport (0,150 1600x900), scissor
    // (0,150)-(1600,1050), on an offscreen target with a depth buffer, last pass of the
    // frame; the D3D11 renderer paints the same pass over the full window and its picture
    // is right. The same happens the other way round in a window wider than 16:9: a
    // 2560x1080 window gets the pillar band (320,0 1920x1080), the picture compressed
    // sideways (user report and census, 2026-09-06). The values arrive through the
    // renderer's generic submit (the caller walk ends in the frame submit, not at a
    // computation), so the correction is applied where they are used: a viewport or
    // scissor that is exactly the window's centred 16:9 band is widened to the whole
    // window. Nothing else matches - at 16:9 there is no band to match, and the gate is
    // the window's aspect.
    struct WindowBand
    {
        bool valid = false;
        int ww = 0, wh = 0;
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;   // the centred 16:9 band
    };

    WindowBand CurrentWindowBand()
    {
        WindowBand b;
        b.ww = RenderPipeline::pWindowResX ? *RenderPipeline::pWindowResX : 0;
        b.wh = RenderPipeline::pWindowResY ? *RenderPipeline::pWindowResY : 0;
        if (b.ww <= 0 || b.wh <= 0) { return b; }
        const int64_t wide = static_cast<int64_t>(b.ww) * 9;
        const int64_t tall = static_cast<int64_t>(b.wh) * 16;
        if (wide < tall)
        {
            // Taller than 16:9: full width, letterboxed.
            b.w = static_cast<float>(b.ww);
            b.h = static_cast<float>(b.ww) * 9.0f / 16.0f;
            b.x = 0.0f;
            b.y = (static_cast<float>(b.wh) - b.h) / 2.0f;
        }
        else if (wide > tall)
        {
            // Wider than 16:9: full height, pillarboxed.
            b.h = static_cast<float>(b.wh);
            b.w = static_cast<float>(b.wh) * 16.0f / 9.0f;
            b.y = 0.0f;
            b.x = (static_cast<float>(b.ww) - b.w) / 2.0f;
        }
        else
        {
            return b;
        }
        b.valid = true;
        return b;
    }

    bool IsWindowBand(float x, float y, float w, float h)
    {
        const WindowBand b = CurrentWindowBand();
        return b.valid && std::fabs(x - b.x) <= 1.0f && std::fabs(y - b.y) <= 1.0f
            && std::fabs(w - b.w) <= 1.0f && std::fabs(h - b.h) <= 1.0f;
    }

    bool IsWindowBandScissor(const D3D12_RECT& r)
    {
        const WindowBand b = CurrentWindowBand();
        return b.valid && std::fabs(static_cast<float>(r.left) - b.x) <= 1.0f && std::fabs(static_cast<float>(r.top) - b.y) <= 1.0f
            && std::fabs(static_cast<float>(r.right) - (b.x + b.w)) <= 1.0f && std::fabs(static_cast<float>(r.bottom) - (b.y + b.h)) <= 1.0f;
    }

    std::atomic<int> g_BandExpandReported { 0 };

    void ReportBandExpand(float x, float y, float w, float h, int ww, int wh)
    {
        if (g_BandExpandReported.fetch_add(1) == 0)
        {
            spdlog::info("MGS4: fit: the engine's 16:9 band ({:.0f},{:.0f} {:.0f}x{:.0f}) is widened to the {}x{} window.", x, y, w, h, ww, wh);
        }
    }

    void STDMETHODCALLTYPE Fit12_RSSetViewports(ID3D12GraphicsCommandList* self, UINT count, const D3D12_VIEWPORT* viewports)
    {
        if (count > 0 && count <= 16 && viewports)
        {
            D3D12_VIEWPORT fitted[16];
            std::memcpy(fitted, viewports, sizeof(D3D12_VIEWPORT) * count);
            bool changed = false;
            if (RenderPipeline::bBandExpand)
            {
                for (UINT i = 0; i < count; i++)
                {
                    if (!IsWindowBand(fitted[i].TopLeftX, fitted[i].TopLeftY, fitted[i].Width, fitted[i].Height)) { continue; }
                    const WindowBand b = CurrentWindowBand();
                    ReportBandExpand(fitted[i].TopLeftX, fitted[i].TopLeftY, fitted[i].Width, fitted[i].Height, b.ww, b.wh);
                    fitted[i].TopLeftX = 0.0f;
                    fitted[i].TopLeftY = 0.0f;
                    fitted[i].Width = static_cast<float>(b.ww);
                    fitted[i].Height = static_cast<float>(b.wh);
                    changed = true;
                }
            }
            if (g_Fit.active && ListBound12(self))
            {
                for (UINT i = 0; i < count; i++) { changed = FitViewport(fitted[i]) || changed; }
            }
            if (changed)
            {
                Fit12_RSSetViewports_hook.stdcall<void>(self, count, fitted);
                return;
            }
        }
        Fit12_RSSetViewports_hook.stdcall<void>(self, count, viewports);
    }

    void STDMETHODCALLTYPE Fit12_RSSetScissorRects(ID3D12GraphicsCommandList* self, UINT count, const D3D12_RECT* rects)
    {
        if (count > 0 && count <= 16 && rects)
        {
            D3D12_RECT fitted[16];
            std::memcpy(fitted, rects, sizeof(D3D12_RECT) * count);
            bool changed = false;
            if (RenderPipeline::bBandExpand)
            {
                for (UINT i = 0; i < count; i++)
                {
                    if (!IsWindowBandScissor(fitted[i])) { continue; }
                    const WindowBand b = CurrentWindowBand();
                    fitted[i] = D3D12_RECT { 0, 0, b.ww, b.wh };
                    changed = true;
                }
            }
            if (g_Fit.active && ListBound12(self))
            {
                for (UINT i = 0; i < count; i++) { changed = FitScissor(fitted[i]) || changed; }
            }
            if (changed)
            {
                Fit12_RSSetScissorRects_hook.stdcall<void>(self, count, fitted);
                return;
            }
        }
        Fit12_RSSetScissorRects_hook.stdcall<void>(self, count, rects);
    }

    void InstallFit12Device(ID3D12CommandQueue* queue, IUnknown* swapChain)
    {
        static bool installed = false;
        if (installed || !queue || !swapChain) { return; }
        installed = true;
        IDXGISwapChain* chain = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&chain))) && chain)
        {
            g_FitSwapChain12 = chain;
            chain->Release();
        }
        ID3D12Device* device = nullptr;
        if (SUCCEEDED(queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device))) && device)
        {
            void** vtable = *reinterpret_cast<void***>(device);
            Fit12_CreateRenderTargetView_hook = safetyhook::create_inline(vtable[20], reinterpret_cast<void*>(Fit12_CreateRenderTargetView));
            device->Release();
        }
        spdlog::info("MGS4: fit: D3D12 CreateRenderTargetView hook {}.", Fit12_CreateRenderTargetView_hook ? "ok" : "FAILED");
    }

    void InstallFit12Lists(ID3D12GraphicsCommandList* list)
    {
        static bool installed = false;
        if (installed || !list) { return; }
        installed = true;
        void** vtable = *reinterpret_cast<void***>(list);
        Fit12_RSSetViewports_hook = safetyhook::create_inline(vtable[21], reinterpret_cast<void*>(Fit12_RSSetViewports));
        Fit12_RSSetScissorRects_hook = safetyhook::create_inline(vtable[22], reinterpret_cast<void*>(Fit12_RSSetScissorRects));
        Fit12_OMSetRenderTargets_hook = safetyhook::create_inline(vtable[46], reinterpret_cast<void*>(Fit12_OMSetRenderTargets));
        spdlog::info("MGS4: fit: D3D12 command list hooks (viewports {}, scissors {}, render targets {}).",
            Fit12_RSSetViewports_hook ? "ok" : "FAILED", Fit12_RSSetScissorRects_hook ? "ok" : "FAILED", Fit12_OMSetRenderTargets_hook ? "ok" : "FAILED");
    }

    void FitOnResize()
    {
        g_BackBuffer11 = nullptr;
        g_BackBufferBound11 = false;
        std::lock_guard lock(g_Fit12Mutex);
        g_BackBufferRtvs12.clear();
        g_BoundLists12.clear();
    }

    // ---- draw census (lab: "drawlog N") ---------------------------------------------------
    //
    // The loading spinner is drawn by nothing the pool append sees - 0-1 appends per upload
    // while it spins (spin219a, 2026-09-04) - yet it sits in the 16:9 logical space like any
    // widget: a 60-unit ring at (1192, 53) of 1280x720, stretched with the window, measured
    // the same at 4:3 and 21:9. So it is looked for one level down, at the command list:
    // every DrawInstanced / DrawIndexedInstanced while the budget lasts, with the present
    // it belongs to and the game frames that issued it. The public render pipeline already
    // holds inline hooks on these two slots; ours go on top (the trampoline relocates the
    // jmp the first hook left), so the chain is ours -> render_pipeline's -> D3D12.
    SafetyHookInline DrawInstanced_hook {};
    SafetyHookInline DrawIndexedInstanced_hook {};
    constexpr size_t kListDrawInstancedSlot = 12;
    constexpr size_t kListDrawIndexedInstancedSlot = 13;
    std::atomic<int> g_DrawLogBudget { 0 };
    std::atomic<uint32_t> g_DrawLogPresent { 0 };

    __declspec(noinline) void DrawLog(const char* kind, UINT count, UINT instances, UINT start, INT base, UINT startInstance)
    {
        if (g_DrawLogBudget.fetch_sub(1) <= 0)
        {
            g_DrawLogBudget.store(0);
            return;
        }
        void* frames[12] {};
        const USHORT n = RtlCaptureStackBackTrace(2, 12, frames, nullptr);
        const auto module = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        std::string chain;
        for (USHORT i = 0; i < n; i++)
        {
            const auto p = reinterpret_cast<uintptr_t>(frames[i]);
            chain += i ? "<" : "";
            chain += (p >= module && p < module + 0x8000000) ? std::format("+{:X}", p - module) : std::string("ext");
        }
        spdlog::info("MGS4: drawlog: p{} {} {} x{} start {} base {} inst {} {}",
            g_DrawLogPresent.load(std::memory_order_relaxed), kind, count, instances, start, base, startInstance, chain);
    }

    void STDMETHODCALLTYPE Hooked_DrawInstancedCensus(ID3D12GraphicsCommandList* self,
        UINT vertexCountPerInstance, UINT instanceCount, UINT startVertexLocation, UINT startInstanceLocation)
    {
        DrawLog("draw", vertexCountPerInstance, instanceCount, startVertexLocation, 0, startInstanceLocation);
        DrawInstanced_hook.stdcall<void>(self, vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
    }

    void STDMETHODCALLTYPE Hooked_DrawIndexedInstancedCensus(ID3D12GraphicsCommandList* self,
        UINT indexCountPerInstance, UINT instanceCount, UINT startIndexLocation, INT baseVertexLocation, UINT startInstanceLocation)
    {
        DrawLog("idx", indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
        DrawIndexedInstanced_hook.stdcall<void>(self, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
    }

#if MGS4E_LAB_BUILD
    // ---- D3D11 context census (lab: Debug Logging) ------------------------------------------
    //
    // Where does the engine put its picture? With a window size that is not the desktop
    // size the picture sits unscaled at the origin of a desktop-sized swap chain (AR-014).
    // To scale it at present time the final blit has to be found: this logs, for the first
    // frames, every render-target bind (with the target's size and whether it is the back
    // buffer), every viewport and every scissor the immediate context is given.
    SafetyHookInline D3D11_OMSetRenderTargets_hook {};
    SafetyHookInline D3D11_RSSetViewports_hook {};
    SafetyHookInline D3D11_RSSetScissorRects_hook {};
    std::atomic<int> g_D3D11CensusBudget { 4000 };
    std::atomic<bool> g_D3D11BackBufferBound { false };
    std::atomic<int> g_D3D11Frame { 0 };
    SafetyHookInline D3D11_Draw_hook {};
    SafetyHookInline D3D11_DrawIndexed_hook {};
    SafetyHookInline D3D11_CopySubresourceRegion_hook {};
    SafetyHookInline D3D11_CopyResource_hook {};
    SafetyHookInline D3D11_ResolveSubresource_hook {};
    D3D11_VIEWPORT g_D3D11LastViewport {};
    bool CensusOn() { const int fr = g_D3D11Frame.load(std::memory_order_relaxed); return fr >= 1 && fr <= 2 && g_D3D11CensusBudget.load(std::memory_order_relaxed) > 0 && g_D3D11CensusBudget.fetch_sub(1) > 0; }
    std::string TexName(ID3D11Resource* res)
    {
        if (!res) { return "null"; }
        ID3D11Texture2D* tex = nullptr;
        std::string s = std::format("{:X}", reinterpret_cast<uintptr_t>(res));
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
        {
            D3D11_TEXTURE2D_DESC d {};
            tex->GetDesc(&d);
            s += std::format(" {}x{} fmt{}", d.Width, d.Height, static_cast<int>(d.Format));
            ID3D11Texture2D* backBuffer = nullptr;
            if (g_Band.swapChain && SUCCEEDED(g_Band.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) && backBuffer)
            {
                if (backBuffer == tex) { s += " BACKBUFFER"; }
                backBuffer->Release();
            }
            tex->Release();
        }
        return s;
    }
    SafetyHookInline D3D11_OMSetRenderTargetsAndUAVs_hook {};
    void STDMETHODCALLTYPE Hooked_D3D11_OMSetRenderTargetsAndUAVs(ID3D11DeviceContext* self, UINT count,
        ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth, UINT uavStart, UINT uavCount,
        ID3D11UnorderedAccessView* const* uavs, const UINT* initialCounts)
    {
        if (count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        {
            ID3D11Resource* r0 = nullptr;
            if (count > 0 && views && views[0]) { views[0]->GetResource(&r0); }
            const std::string name = TexName(r0);
            g_D3D11BackBufferBound.store(name.find("BACKBUFFER") != std::string::npos, std::memory_order_relaxed);
            if (CensusOn()) { spdlog::info("MGS4: d3d11 census: f{} OMSetRenderTargetsAndUAVs count={} rt0={} depth={} uavs={}", g_D3D11Frame.load(), count, name, depth ? "yes" : "no", uavCount); }
            if (r0) { r0->Release(); }
        }
        D3D11_OMSetRenderTargetsAndUAVs_hook.stdcall<void>(self, count, views, depth, uavStart, uavCount, uavs, initialCounts);
    }

    void STDMETHODCALLTYPE Hooked_D3D11_Draw(ID3D11DeviceContext* self, UINT vertexCount, UINT start)
    {
        if (CensusOn()) { spdlog::info("MGS4: d3d11 census: f{} Draw {} vp=({:.0f},{:.0f} {:.0f}x{:.0f}){}", g_D3D11Frame.load(), vertexCount, g_D3D11LastViewport.TopLeftX, g_D3D11LastViewport.TopLeftY, g_D3D11LastViewport.Width, g_D3D11LastViewport.Height, g_D3D11BackBufferBound.load() ? " [backbuffer]" : ""); }
        D3D11_Draw_hook.stdcall<void>(self, vertexCount, start);
    }
    void STDMETHODCALLTYPE Hooked_D3D11_DrawIndexed(ID3D11DeviceContext* self, UINT indexCount, UINT start, INT base)
    {
        if (CensusOn()) { spdlog::info("MGS4: d3d11 census: f{} DrawIndexed {} vp=({:.0f},{:.0f} {:.0f}x{:.0f}){}", g_D3D11Frame.load(), indexCount, g_D3D11LastViewport.TopLeftX, g_D3D11LastViewport.TopLeftY, g_D3D11LastViewport.Width, g_D3D11LastViewport.Height, g_D3D11BackBufferBound.load() ? " [backbuffer]" : ""); }
        D3D11_DrawIndexed_hook.stdcall<void>(self, indexCount, start, base);
    }
    void STDMETHODCALLTYPE Hooked_D3D11_CopySubresourceRegion(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSub, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box)
    {
        if (CensusOn()) { spdlog::info("MGS4: d3d11 census: f{} CopySubresourceRegion dst {} at ({},{}) src {} box {}", g_D3D11Frame.load(), TexName(dst), x, y, TexName(src), box ? std::format("({},{})-({},{})", box->left, box->top, box->right, box->bottom) : "none"); }
        D3D11_CopySubresourceRegion_hook.stdcall<void>(self, dst, dstSub, x, y, z, src, srcSub, box);
    }
    void STDMETHODCALLTYPE Hooked_D3D11_CopyResource(ID3D11DeviceContext* self, ID3D11Resource* dst, ID3D11Resource* src)
    {
        if (CensusOn()) { spdlog::info("MGS4: d3d11 census: f{} CopyResource dst {} src {}", g_D3D11Frame.load(), TexName(dst), TexName(src)); }
        D3D11_CopyResource_hook.stdcall<void>(self, dst, src);
    }
    void STDMETHODCALLTYPE Hooked_D3D11_ResolveSubresource(ID3D11DeviceContext* self, ID3D11Resource* dst, UINT dstSub, ID3D11Resource* src, UINT srcSub, DXGI_FORMAT format)
    {
        if (CensusOn()) { spdlog::info("MGS4: d3d11 census: f{} ResolveSubresource dst {} src {}", g_D3D11Frame.load(), TexName(dst), TexName(src)); }
        D3D11_ResolveSubresource_hook.stdcall<void>(self, dst, dstSub, src, srcSub, format);
    }

    void STDMETHODCALLTYPE Hooked_D3D11_OMSetRenderTargets(ID3D11DeviceContext* self, UINT count,
        ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth)
    {
        bool back = false;
        UINT w = 0, h = 0;
        if (count > 0 && views && views[0])
        {
            ID3D11Resource* res = nullptr;
            views[0]->GetResource(&res);
            if (res)
            {
                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex)
                {
                    D3D11_TEXTURE2D_DESC d {};
                    tex->GetDesc(&d);
                    w = d.Width; h = d.Height;
                    ID3D11Texture2D* backBuffer = nullptr;
                    if (g_Band.swapChain && SUCCEEDED(g_Band.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) && backBuffer)
                    {
                        back = backBuffer == tex;
                        backBuffer->Release();
                    }
                    tex->Release();
                }
                res->Release();
            }
        }
        g_D3D11BackBufferBound.store(back, std::memory_order_relaxed);
        if (CensusOn())
        {
            ID3D11Resource* r0 = nullptr;
            if (count > 0 && views && views[0]) { views[0]->GetResource(&r0); }
            spdlog::info("MGS4: d3d11 census: f{} OMSetRenderTargets count={} rtv0={:X} rt0={} depth={}",
                g_D3D11Frame.load(), count, reinterpret_cast<uintptr_t>(count > 0 && views ? views[0] : nullptr), TexName(r0), depth ? "yes" : "no");
            if (r0) { r0->Release(); }
        }
        D3D11_OMSetRenderTargets_hook.stdcall<void>(self, count, views, depth);
    }

    void STDMETHODCALLTYPE Hooked_D3D11_RSSetViewports(ID3D11DeviceContext* self, UINT count, const D3D11_VIEWPORT* viewports)
    {
        if (count > 0 && viewports) { g_D3D11LastViewport = viewports[0]; }
        if (CensusOn() && count > 0 && viewports)
        {
            spdlog::info("MGS4: d3d11 census: f{} RSSetViewports count={} vp0=({:.0f},{:.0f} {:.0f}x{:.0f}){}",
                g_D3D11Frame.load(), count, viewports[0].TopLeftX, viewports[0].TopLeftY, viewports[0].Width, viewports[0].Height,
                g_D3D11BackBufferBound.load(std::memory_order_relaxed) ? " [backbuffer bound]" : "");
        }
        D3D11_RSSetViewports_hook.stdcall<void>(self, count, viewports);
    }

    void STDMETHODCALLTYPE Hooked_D3D11_RSSetScissorRects(ID3D11DeviceContext* self, UINT count, const D3D11_RECT* rects)
    {
        if (CensusOn() && count > 0 && rects)
        {
            spdlog::info("MGS4: d3d11 census: f{} RSSetScissorRects count={} r0=({},{} {},{}){}",
                g_D3D11Frame.load(), count, rects[0].left, rects[0].top, rects[0].right, rects[0].bottom,
                g_D3D11BackBufferBound.load(std::memory_order_relaxed) ? " [backbuffer bound]" : "");
        }
        D3D11_RSSetScissorRects_hook.stdcall<void>(self, count, rects);
    }

    void InstallD3D11Census(ID3D11DeviceContext* context)
    {
        static bool installed = false;
        if (installed || !context || !mgs4e::log::Verbose()) { return; }
        installed = true;
        void** vtable = *reinterpret_cast<void***>(context);
        D3D11_OMSetRenderTargets_hook = safetyhook::create_inline(vtable[33], reinterpret_cast<void*>(Hooked_D3D11_OMSetRenderTargets));
        D3D11_RSSetViewports_hook = safetyhook::create_inline(vtable[44], reinterpret_cast<void*>(Hooked_D3D11_RSSetViewports));
        D3D11_RSSetScissorRects_hook = safetyhook::create_inline(vtable[45], reinterpret_cast<void*>(Hooked_D3D11_RSSetScissorRects));
        D3D11_DrawIndexed_hook = safetyhook::create_inline(vtable[12], reinterpret_cast<void*>(Hooked_D3D11_DrawIndexed));
        D3D11_OMSetRenderTargetsAndUAVs_hook = safetyhook::create_inline(vtable[34], reinterpret_cast<void*>(Hooked_D3D11_OMSetRenderTargetsAndUAVs));
        D3D11_Draw_hook = safetyhook::create_inline(vtable[13], reinterpret_cast<void*>(Hooked_D3D11_Draw));
        D3D11_CopySubresourceRegion_hook = safetyhook::create_inline(vtable[46], reinterpret_cast<void*>(Hooked_D3D11_CopySubresourceRegion));
        D3D11_CopyResource_hook = safetyhook::create_inline(vtable[47], reinterpret_cast<void*>(Hooked_D3D11_CopyResource));
        D3D11_ResolveSubresource_hook = safetyhook::create_inline(vtable[57], reinterpret_cast<void*>(Hooked_D3D11_ResolveSubresource));
        spdlog::info("MGS4: d3d11 census: context hooks OMSetRenderTargets {}, RSSetViewports {}, RSSetScissorRects {}.",
            D3D11_OMSetRenderTargets_hook ? "ok" : "FAILED", D3D11_RSSetViewports_hook ? "ok" : "FAILED", D3D11_RSSetScissorRects_hook ? "ok" : "FAILED");
    }
#endif

#if MGS4E_LAB_BUILD
    // ---- display census (lab: Debug Logging) ------------------------------------------------
    //
    // Where does the engine get the size it resizes its chain to? It is not the ecf, not the
    // launcher's save, not another mod, not the desktop (all ruled out 2026-09-05). This logs
    // what it asks the display APIs: mode enumeration, screen metrics, device caps, and the
    // DXGI output's mode list, with the caller's address in the game image.
    SafetyHookInline EnumDisplaySettingsW_hook {};
    SafetyHookInline EnumDisplaySettingsExW_hook {};
    SafetyHookInline GetSystemMetrics_hook {};
    SafetyHookInline GetDeviceCaps_hook {};
    SafetyHookInline EnumAdapters1_hook {};
    SafetyHookInline EnumOutputs_hook {};
    SafetyHookInline GetDisplayModeList_hook {};
    SafetyHookInline OutputGetDesc_hook {};
    std::atomic<int> g_DisplayCensusBudget { 400 };

    std::string CallerTag(void* ret)
    {
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        const auto p = reinterpret_cast<uintptr_t>(ret);
        return (p >= base && p < base + 0x8000000) ? std::format("mgs4+{:X}", p - base) : std::format("ext {:X}", p);
    }
    bool DisplayCensusOn() { return g_DisplayCensusBudget.load(std::memory_order_relaxed) > 0 && g_DisplayCensusBudget.fetch_sub(1) > 0; }

    BOOL WINAPI Hooked_EnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW* dm)
    {
        const BOOL r = EnumDisplaySettingsW_hook.stdcall<BOOL>(device, mode, dm);
        // The walk is hundreds of entries: log the current/registry queries, each new largest
        // mode, and the end of the walk.
        static DWORD maxArea = 0;
        const DWORD area = (r && dm) ? dm->dmPelsWidth * dm->dmPelsHeight : 0;
        const bool special = static_cast<int>(mode) < 0;
        const bool newMax = r && area > maxArea;
        if (newMax) { maxArea = area; }
        if (!r) { maxArea = 0; }
        if ((special || newMax || !r) && DisplayCensusOn())
        {
            spdlog::info("MGS4: display census: EnumDisplaySettingsW(mode {}) -> {} {}x{} @{} from {}", static_cast<int>(mode), r ? "ok" : "fail",
                dm ? dm->dmPelsWidth : 0, dm ? dm->dmPelsHeight : 0, dm ? dm->dmDisplayFrequency : 0, CallerTag(_ReturnAddress()));
        }
        return r;
    }
    BOOL WINAPI Hooked_EnumDisplaySettingsExW(LPCWSTR device, DWORD mode, DEVMODEW* dm, DWORD flags)
    {
        const BOOL r = EnumDisplaySettingsExW_hook.stdcall<BOOL>(device, mode, dm, flags);
        if (DisplayCensusOn())
        {
            spdlog::info("MGS4: display census: EnumDisplaySettingsExW(mode {}, flags {}) -> {} {}x{} from {}", static_cast<int>(mode), flags, r ? "ok" : "fail",
                dm ? dm->dmPelsWidth : 0, dm ? dm->dmPelsHeight : 0, CallerTag(_ReturnAddress()));
        }
        return r;
    }
    int WINAPI Hooked_GetSystemMetrics(int index)
    {
        const int r = GetSystemMetrics_hook.stdcall<int>(index);
        if ((index == SM_CXSCREEN || index == SM_CYSCREEN || index == SM_CXVIRTUALSCREEN || index == SM_CYVIRTUALSCREEN) && DisplayCensusOn())
        {
            spdlog::info("MGS4: display census: GetSystemMetrics({}) -> {} from {}", index, r, CallerTag(_ReturnAddress()));
        }
        return r;
    }
    int WINAPI Hooked_GetDeviceCaps(HDC dc, int index)
    {
        const int r = GetDeviceCaps_hook.stdcall<int>(dc, index);
        if ((index == HORZRES || index == VERTRES || index == DESKTOPHORZRES || index == DESKTOPVERTRES) && DisplayCensusOn())
        {
            spdlog::info("MGS4: display census: GetDeviceCaps({}) -> {} from {}", index, r, CallerTag(_ReturnAddress()));
        }
        return r;
    }
    HRESULT STDMETHODCALLTYPE Hooked_GetDisplayModeList(IDXGIOutput* self, DXGI_FORMAT format, UINT flags, UINT* count, DXGI_MODE_DESC* modes)
    {
        const HRESULT r = GetDisplayModeList_hook.stdcall<HRESULT>(self, format, flags, count, modes);
        if (DisplayCensusOn())
        {
            std::string biggest = "n/a";
            if (modes && count && *count) { biggest = std::format("first {}x{} last {}x{}", modes[0].Width, modes[0].Height, modes[*count - 1].Width, modes[*count - 1].Height); }
            spdlog::info("MGS4: display census: IDXGIOutput::GetDisplayModeList(fmt {}) -> {} modes, {} from {}", static_cast<int>(format), count ? *count : 0, biggest, CallerTag(_ReturnAddress()));
        }
        return r;
    }
    HRESULT STDMETHODCALLTYPE Hooked_OutputGetDesc(IDXGIOutput* self, DXGI_OUTPUT_DESC* desc)
    {
        const HRESULT r = OutputGetDesc_hook.stdcall<HRESULT>(self, desc);
        if (DisplayCensusOn() && desc)
        {
            spdlog::info("MGS4: display census: IDXGIOutput::GetDesc -> desktop ({},{})-({},{}) from {}", desc->DesktopCoordinates.left, desc->DesktopCoordinates.top,
                desc->DesktopCoordinates.right, desc->DesktopCoordinates.bottom, CallerTag(_ReturnAddress()));
        }
        return r;
    }
    void HookOutput(IDXGIOutput* output)
    {
        static bool hooked = false;
        if (hooked || !output) { return; }
        hooked = true;
        void** vtable = *reinterpret_cast<void***>(output);
        OutputGetDesc_hook = safetyhook::create_inline(vtable[7], reinterpret_cast<void*>(Hooked_OutputGetDesc));
        GetDisplayModeList_hook = safetyhook::create_inline(vtable[8], reinterpret_cast<void*>(Hooked_GetDisplayModeList));
    }
    HRESULT STDMETHODCALLTYPE Hooked_EnumOutputs(IDXGIAdapter* self, UINT index, IDXGIOutput** output)
    {
        const HRESULT r = EnumOutputs_hook.stdcall<HRESULT>(self, index, output);
        if (SUCCEEDED(r) && output && *output) { HookOutput(*output); if (DisplayCensusOn()) { spdlog::info("MGS4: display census: EnumOutputs({}) from {}", index, CallerTag(_ReturnAddress())); } }
        return r;
    }
    void HookAdapter(IDXGIAdapter* adapter)
    {
        static bool hooked = false;
        if (hooked || !adapter) { return; }
        hooked = true;
        void** vtable = *reinterpret_cast<void***>(adapter);
        EnumOutputs_hook = safetyhook::create_inline(vtable[7], reinterpret_cast<void*>(Hooked_EnumOutputs));
    }
    HRESULT STDMETHODCALLTYPE Hooked_EnumAdapters1(IDXGIFactory1* self, UINT index, IDXGIAdapter1** adapter)
    {
        const HRESULT r = EnumAdapters1_hook.stdcall<HRESULT>(self, index, adapter);
        if (SUCCEEDED(r) && adapter && *adapter) { HookAdapter(*adapter); }
        return r;
    }
    void InstallDisplayCensus()
    {
        if (!mgs4e::log::Verbose()) { return; }
        if (const HMODULE user32 = GetModuleHandleW(L"user32.dll"))
        {
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsW"))) { EnumDisplaySettingsW_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_EnumDisplaySettingsW)); }
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsExW"))) { EnumDisplaySettingsExW_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_EnumDisplaySettingsExW)); }
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(user32, "GetSystemMetrics"))) { GetSystemMetrics_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_GetSystemMetrics)); }
        }
        if (const HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll"))
        {
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(gdi32, "GetDeviceCaps"))) { GetDeviceCaps_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_GetDeviceCaps)); }
        }
        spdlog::info("MGS4: display census: EnumDisplaySettingsW {}, ExW {}, GetSystemMetrics {}, GetDeviceCaps {}.",
            EnumDisplaySettingsW_hook ? "ok" : "no", EnumDisplaySettingsExW_hook ? "ok" : "no", GetSystemMetrics_hook ? "ok" : "no", GetDeviceCaps_hook ? "ok" : "no");
    }
    void HookFactoryForCensus(IUnknown* factory)
    {
        static bool hooked = false;
        if (hooked || !factory || !mgs4e::log::Verbose()) { return; }
        hooked = true;
        IDXGIFactory1* one = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&one))) && one)
        {
            void** vtable = *reinterpret_cast<void***>(one);
            EnumAdapters1_hook = safetyhook::create_inline(vtable[12], reinterpret_cast<void*>(Hooked_EnumAdapters1));
            one->Release();
            spdlog::info("MGS4: display census: EnumAdapters1 {}.", EnumAdapters1_hook ? "ok" : "FAILED");
        }
    }

    SafetyHookInline C12_RSSetViewports_hook {};
    SafetyHookInline C12_RSSetScissorRects_hook {};
    SafetyHookInline C12_OMSetRenderTargets_hook {};
    std::atomic<int> g_C12Budget { 600 };
    bool C12On() { const int fr = g_D3D11Frame.load(std::memory_order_relaxed); return fr >= 150 && fr <= 151 && g_C12Budget.load(std::memory_order_relaxed) > 0 && g_C12Budget.fetch_sub(1) > 0; }
    void STDMETHODCALLTYPE C12_OMSetRenderTargets(ID3D12GraphicsCommandList* self, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* handles, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* depth)
    {
        if (C12On())
        {
            bool back = false;
            { std::lock_guard lock(g_Fit12Mutex); back = count > 0 && handles && std::find(g_BackBufferRtvs12.begin(), g_BackBufferRtvs12.end(), handles[0].ptr) != g_BackBufferRtvs12.end(); }
            spdlog::info("MGS4: d3d12 census: f{} list {:X} OMSetRenderTargets count={} rtv0={:X} ({}){} depth={}", g_D3D11Frame.load(), reinterpret_cast<uintptr_t>(self), count,
                (count > 0 && handles) ? handles[0].ptr : 0, (count > 0 && handles) ? RtvSize12(handles[0].ptr) : "-", back ? " BACKBUFFER" : "", depth ? "yes" : "no");
        }
        C12_OMSetRenderTargets_hook.stdcall<void>(self, count, handles, single, depth);
    }
#if MGS4E_LAB_BUILD
    // Who sets the band? A few return addresses up from the RSSetViewports call, logged once.
    std::atomic<int> g_BandWalkBudget { 2 };
    void LogBandCallers()
    {
        if (g_BandWalkBudget.load(std::memory_order_relaxed) <= 0 || g_BandWalkBudget.fetch_sub(1) <= 0) { return; }
        CONTEXT c {};
        RtlCaptureContext(&c);
        std::string walk;
        for (int i = 0; i < 12; i++)
        {
            DWORD64 imageBase = 0;
            auto* entry = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
            if (!entry)
            {
                if (!mgs4e::mem::Readable(reinterpret_cast<const void*>(c.Rsp), sizeof(DWORD64))) { break; }
                std::memcpy(&c.Rip, reinterpret_cast<const void*>(c.Rsp), sizeof(c.Rip));
                c.Rsp += 8;
            }
            else
            {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, entry, &c, &handlerData, &establisher, nullptr);
            }
            if (!c.Rip) { break; }
            walk += (walk.empty() ? "" : " < ") + CallerTag(reinterpret_cast<void*>(c.Rip));
        }
        spdlog::info("MGS4: d3d12 census: band viewport callers: {}", walk);
    }
#endif

    void STDMETHODCALLTYPE C12_RSSetViewports(ID3D12GraphicsCommandList* self, UINT count, const D3D12_VIEWPORT* vps)
    {
#if MGS4E_LAB_BUILD
        if (count > 0 && vps && IsWindowBand(vps[0].TopLeftX, vps[0].TopLeftY, vps[0].Width, vps[0].Height))
        {
            LogBandCallers();
        }
#endif
        if (C12On() && count > 0 && vps)
        {
            spdlog::info("MGS4: d3d12 census: f{} list {:X} RSSetViewports count={} vp0=({:.0f},{:.0f} {:.0f}x{:.0f}){}", g_D3D11Frame.load(), reinterpret_cast<uintptr_t>(self), count,
                vps[0].TopLeftX, vps[0].TopLeftY, vps[0].Width, vps[0].Height, ListBound12(self) ? " [backbuffer bound]" : "");
        }
        C12_RSSetViewports_hook.stdcall<void>(self, count, vps);
    }
    void STDMETHODCALLTYPE C12_RSSetScissorRects(ID3D12GraphicsCommandList* self, UINT count, const D3D12_RECT* rects)
    {
        if (C12On() && count > 0 && rects)
        {
            spdlog::info("MGS4: d3d12 census: f{} list {:X} RSSetScissorRects count={} r0=({},{} {},{}){}", g_D3D11Frame.load(), reinterpret_cast<uintptr_t>(self), count,
                rects[0].left, rects[0].top, rects[0].right, rects[0].bottom, ListBound12(self) ? " [backbuffer bound]" : "");
        }
        C12_RSSetScissorRects_hook.stdcall<void>(self, count, rects);
    }

    void InstallDrawCensus(ID3D12GraphicsCommandList* list)
    {
        static bool installed = false;
        if (installed || !list) { return; }
        installed = true;
        void** vtable = *reinterpret_cast<void***>(list);
        C12_RSSetViewports_hook = safetyhook::create_inline(vtable[21], reinterpret_cast<void*>(C12_RSSetViewports));
        C12_RSSetScissorRects_hook = safetyhook::create_inline(vtable[22], reinterpret_cast<void*>(C12_RSSetScissorRects));
        C12_OMSetRenderTargets_hook = safetyhook::create_inline(vtable[46], reinterpret_cast<void*>(C12_OMSetRenderTargets));
        DrawInstanced_hook = safetyhook::create_inline(vtable[kListDrawInstancedSlot], reinterpret_cast<void*>(Hooked_DrawInstancedCensus));
        DrawIndexedInstanced_hook = safetyhook::create_inline(vtable[kListDrawIndexedInstancedSlot], reinterpret_cast<void*>(Hooked_DrawIndexedInstancedCensus));
        spdlog::info("MGS4: letterbox: draw census hooks (DrawInstanced {}, DrawIndexedInstanced {}).",
            DrawInstanced_hook ? "ok" : "FAILED", DrawIndexedInstanced_hook ? "ok" : "FAILED");
    }

#endif

    void BandWaitIdle(BandRenderer& b)
    {
        if (!b.fence || !b.queue) { return; }
        const UINT64 value = ++b.fenceCounter;
        if (SUCCEEDED(b.queue->Signal(b.fence, value)) && b.fence->GetCompletedValue() < value)
        {
            b.fence->SetEventOnCompletion(value, b.fenceEvent);
            WaitForSingleObject(b.fenceEvent, 2000);
        }
    }

    void BandRelease(BandRenderer& b)
    {
        BandWaitIdle(b);
        for (auto*& r : b.buffers) { SafeRelease(r); }
        for (auto*& a : b.allocators) { SafeRelease(a); }
        b.buffers.clear();
        b.allocators.clear();
        b.fenceValues.clear();
        SafeRelease(b.list);
        SafeRelease(b.rtvHeap);
        SafeRelease(b.fence);
        if (b.fenceEvent) { CloseHandle(b.fenceEvent); b.fenceEvent = nullptr; }
        SafeRelease(b.device);
        SafeRelease(b.queue);
        SafeRelease(b.context11);
        SafeRelease(b.device11);
        FitOnResize();
        SafeRelease(b.swapChain);
        b.width = b.height = 0;
    }

    bool BandSetup(BandRenderer& b, IDXGISwapChain* swapChain)
    {
        BandRelease(b);
        if (FAILED(swapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&b.swapChain))) || !b.swapChain)
        {
            spdlog::error("MGS4: letterbox: the swap chain is not IDXGISwapChain3."); return false;
        }
        DXGI_SWAP_CHAIN_DESC desc {};
        if (FAILED(b.swapChain->GetDesc(&desc)) || desc.BufferCount == 0) { spdlog::error("MGS4: letterbox: GetDesc failed."); return false; }
        b.width = desc.BufferDesc.Width;
        b.height = desc.BufferDesc.Height;

        // D3D11, the renderer the game ships set to: the bands are cleared through the
        // immediate context at each present, with the back buffer fetched and released on
        // the spot, so nothing of ours is outstanding when the game resizes the chain.
        if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&b.device11))) && b.device11)
        {
            ID3D11DeviceContext* context = nullptr;
            b.device11->GetImmediateContext(&context);
            if (!context || FAILED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&b.context11))) || !b.context11)
            {
                SafeRelease(context);
                spdlog::error("MGS4: letterbox: the D3D11 device has no ID3D11DeviceContext1 (D3D 11.1 is needed for ClearView)."); return false;
            }
            {
                ID3D11Texture2D* back = nullptr;
                if (SUCCEEDED(b.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) && back)
                {
                    g_BackBuffer11 = back;
                    back->Release();
                }
            }
            InstallFit11(b.context11);
#if MGS4E_LAB_BUILD
            InstallD3D11Census(b.context11);
#endif
            context->Release();
            spdlog::info("MGS4: letterbox: band renderer ready on a {}x{} D3D11 swap chain, {} buffers, format {}.",
                b.width, b.height, desc.BufferCount, static_cast<int>(desc.BufferDesc.Format));
            return true;
        }

        // D3D12: a command list of our own on the game's present queue.
        if (!g_PresentQueue) { spdlog::error("MGS4: letterbox: no command queue captured at swap chain creation."); return false; }
        if (FAILED(swapChain->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&b.device))) || !b.device)
        {
            spdlog::error("MGS4: letterbox: the swap chain's device is neither D3D11 nor D3D12."); return false;
        }
        b.queue = g_PresentQueue;
        b.queue->AddRef();

        D3D12_DESCRIPTOR_HEAP_DESC heap {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = desc.BufferCount;
        if (FAILED(b.device->CreateDescriptorHeap(&heap, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&b.rtvHeap))))
        {
            spdlog::error("MGS4: letterbox: RTV heap creation failed."); return false;
        }
        b.rtvStride = b.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = b.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < desc.BufferCount; i++)
        {
            ID3D12Resource* buffer = nullptr;
            if (FAILED(b.swapChain->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&buffer))) || !buffer)
            {
                spdlog::error("MGS4: letterbox: GetBuffer({}) failed.", i); return false;
            }
            b.buffers.push_back(buffer);
            b.device->CreateRenderTargetView(buffer, nullptr, handle);
            handle.ptr += b.rtvStride;

            ID3D12CommandAllocator* allocator = nullptr;
            if (FAILED(b.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&allocator))))
            {
                spdlog::error("MGS4: letterbox: allocator creation failed."); return false;
            }
            b.allocators.push_back(allocator);
            b.fenceValues.push_back(0);
        }
        if (FAILED(b.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, b.allocators[0], nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&b.list))))
        {
            spdlog::error("MGS4: letterbox: command list creation failed."); return false;
        }
        b.list->Close();
        InstallFit12Lists(b.list);
#if MGS4E_LAB_BUILD
        if (mgs4e::log::Verbose()) { InstallDrawCensus(b.list); }
#endif
        if (FAILED(b.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&b.fence))))
        {
            spdlog::error("MGS4: letterbox: fence creation failed."); return false;
        }
        b.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        spdlog::info("MGS4: letterbox: band renderer ready on a {}x{} swap chain, {} buffers, format {}.",
            b.width, b.height, desc.BufferCount, static_cast<int>(desc.BufferDesc.Format));
        return true;
    }

    // The band rects for an output, in back-buffer pixels. Zero rects at 16:9.
    UINT BandRects(UINT width, UINT height, D3D12_RECT rects[2])
    {
        const uint64_t wide = static_cast<uint64_t>(width) * 9;
        const uint64_t tall = static_cast<uint64_t>(height) * 16;
        if (wide > tall)
        {
            const LONG safeWidth = static_cast<LONG>(static_cast<uint64_t>(height) * 16 / 9);
            const LONG left = (static_cast<LONG>(width) - safeWidth) / 2;
            rects[0] = { 0, 0, left, static_cast<LONG>(height) };
            rects[1] = { left + safeWidth, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
            return 2;
        }
        if (wide < tall)
        {
            const LONG safeHeight = static_cast<LONG>(static_cast<uint64_t>(width) * 9 / 16);
            const LONG top = (static_cast<LONG>(height) - safeHeight) / 2;
            rects[0] = { 0, 0, static_cast<LONG>(width), top };
            rects[1] = { 0, top + safeHeight, static_cast<LONG>(width), static_cast<LONG>(height) };
            return 2;
        }
        return 0;
    }

    bool BandsWanted()
    {
        switch (g_BandMode.load())
        {
            case 1: return true;
            case 2: return false;
            default:
            {
                // Both the ladder and the grace are stepped every present, whichever
                // switches are on: the ladder's dismissal rung starts the grace the cutscene
                // masking relies on, and the grace ends on the first present without the
                // intro camera. Ladder first - the grace must be read after the rung on the
                // same present (one painted frame otherwise; split_b, 2026-09-04).
                const bool boot = BootPhaseLive();
                const bool grace = PostLoadGrace();
                // With the menus unmasked, the plate itself is left alone, and so is the
                // intro camera that starts under it at the press: the dismissal identity
                // lands ~0.6 s after the key, and the camera was back for ~7 presents
                // before the grace began (split_a, 2026-09-04) - a visible blink.
                const bool plateUp = static_cast<BootStage>(g_BootStage.load(std::memory_order_relaxed)) == BootStage::PlateUp;
                return (bMenuMasking && (boot || ScreenLive()))
                    || (bCutsceneMasking && CutsceneCameraLive() && !grace && !plateUp);
            }
        }
    }

    void DrawBands(IDXGISwapChain* swapChain)
    {
        std::lock_guard lock(g_BandMutex);
        BandRenderer& b = g_Band;
        b.presents++;
        if (b.failed) { return; }

        if (!b.swapChain || b.swapChain != static_cast<void*>(swapChain))
        {
            // First use, or a different swap chain: (re)build against this one. Pointer
            // identity is on the IUnknown-derived object, which is what GetBuffer etc. use.
            IDXGISwapChain3* probe = nullptr;
            const bool same = b.swapChain && SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&probe))) && probe == b.swapChain;
            if (probe) { probe->Release(); }
            if (!same && !BandSetup(b, swapChain))
            {
                b.failed = true;
                BandRelease(b);
                spdlog::error("MGS4: letterbox: bands disabled for this session.");
                return;
            }
        }

        // The fit for the next frame's draws (AR-014): the picture is the engine's window
        // size, scaled and centred into the chain. Logged when it changes.
        {
            const FitRect fit = ComputeFit(b.width, b.height);
            if (fit.active != g_Fit.active || fit.w != g_Fit.w || fit.h != g_Fit.h || fit.ww != g_Fit.ww || fit.wh != g_Fit.wh)
            {
                if (fit.active)
                {
                    spdlog::info("MGS4: fit: {}x{} picture scaled x{:.4f} into a {}x{} rect at ({},{}) of the {}x{} chain.",
                        fit.ww, fit.wh, fit.scale, fit.w, fit.h, static_cast<int>(fit.ox), static_cast<int>(fit.oy), b.width, b.height);
                }
                else
                {
                    spdlog::info("MGS4: fit: window {}x{} is the chain size; no scaling.", fit.ww, fit.wh);
                }
            }
            g_Fit = fit;
        }

        // Outside the fitted rect the chain holds whatever was there - under D3D12 the
        // engine's own uncleared margins are copied over 1:1 - so it is cleared to black
        // every present the fit is active. The bands then frame the 16:9 picture inside
        // the fitted rect.
        D3D12_RECT rects[6] {};
        UINT count = 0;
        if (g_Fit.active)
        {
            const LONG cw = static_cast<LONG>(b.width), chh = static_cast<LONG>(b.height);
            const LONG ox = static_cast<LONG>(g_Fit.ox), oy = static_cast<LONG>(g_Fit.oy);
            const LONG fw = static_cast<LONG>(g_Fit.w), fh = static_cast<LONG>(g_Fit.h);
            if (oy > 0) { rects[count++] = { 0, 0, cw, oy }; }
            if (oy + fh < chh) { rects[count++] = { 0, oy + fh, cw, chh }; }
            if (ox > 0) { rects[count++] = { 0, 0, ox, chh }; }
            if (ox + fw < cw) { rects[count++] = { ox + fw, 0, cw, chh }; }
        }
        if (BandsWanted())
        {
            D3D12_RECT band[2] {};
            const UINT bands = BandRects(g_Fit.w, g_Fit.h, band);
            for (UINT i = 0; i < bands; i++)
            {
                band[i].left += static_cast<LONG>(g_Fit.ox);
                band[i].right += static_cast<LONG>(g_Fit.ox);
                band[i].top += static_cast<LONG>(g_Fit.oy);
                band[i].bottom += static_cast<LONG>(g_Fit.oy);
                rects[count++] = band[i];
            }
            g_BandLastPainted[0] = g_Fit.w;
            g_BandLastPainted[1] = g_Fit.h;
            g_BandLastRects[0] = band[0];
            g_BandLastRects[1] = band[1];
        }
        if (count == 0) { return; }

        if (b.device11)
        {
            // D3D11 renders into buffer 0 whatever the swap effect; ClearView takes the rects.
            ID3D11Texture2D* back = nullptr;
            if (FAILED(b.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) { return; }
            ID3D11RenderTargetView* rtv = nullptr;
            if (SUCCEEDED(b.device11->CreateRenderTargetView(back, nullptr, &rtv)) && rtv)
            {
                const float black[4] { 0.0f, 0.0f, 0.0f, 1.0f };
                b.context11->ClearView(rtv, black, reinterpret_cast<const D3D11_RECT*>(rects), count);
                rtv->Release();
                b.cleared++;
            }
            back->Release();
            return;
        }

        const UINT index = b.swapChain->GetCurrentBackBufferIndex();
        g_BandLastIndex = index;
        if (index >= b.buffers.size()) { return; }
        if (b.fence->GetCompletedValue() < b.fenceValues[index])
        {
            b.fence->SetEventOnCompletion(b.fenceValues[index], b.fenceEvent);
            WaitForSingleObject(b.fenceEvent, 1000);
        }
        if (FAILED(b.allocators[index]->Reset()) || FAILED(b.list->Reset(b.allocators[index], nullptr))) { return; }

        D3D12_RESOURCE_BARRIER barrier {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = b.buffers[index];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.list->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = b.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(index) * b.rtvStride;
        const float black[4] { 0.0f, 0.0f, 0.0f, 1.0f };
        b.list->ClearRenderTargetView(rtv, black, count, rects);

        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        b.list->ResourceBarrier(1, &barrier);
        if (FAILED(b.list->Close())) { return; }

        ID3D12CommandList* lists[] { b.list };
        b.queue->ExecuteCommandLists(1, lists);
        b.fenceValues[index] = ++b.fenceCounter;
        b.queue->Signal(b.fence, b.fenceCounter);
        b.cleared++;
    }

    HRESULT STDMETHODCALLTYPE Hooked_Present(IDXGISwapChain* self, UINT syncInterval, UINT flags)
    {
        if ((flags & DXGI_PRESENT_TEST) == 0)
        {
#if MGS4E_LAB_BUILD
            const uint64_t t0 = PerfTicks();
#endif
            DrawBands(self);
#if MGS4E_LAB_BUILD
            g_Perf.bandTicks.fetch_add(PerfTicks() - t0, std::memory_order_relaxed);
            PerfPresent();
            g_D3D11Frame.fetch_add(1, std::memory_order_relaxed);
#endif
            g_DrawLogPresent.fetch_add(1, std::memory_order_relaxed);
        }
        return Present_hook.stdcall<HRESULT>(self, syncInterval, flags);
    }

    HRESULT STDMETHODCALLTYPE Hooked_Present1(IDXGISwapChain1* self, UINT syncInterval, UINT flags,
        const DXGI_PRESENT_PARAMETERS* parameters)
    {
        if ((flags & DXGI_PRESENT_TEST) == 0)
        {
#if MGS4E_LAB_BUILD
            const uint64_t t0 = PerfTicks();
#endif
            DrawBands(self);
#if MGS4E_LAB_BUILD
            g_Perf.bandTicks.fetch_add(PerfTicks() - t0, std::memory_order_relaxed);
            PerfPresent();
            g_D3D11Frame.fetch_add(1, std::memory_order_relaxed);
#endif
            g_DrawLogPresent.fetch_add(1, std::memory_order_relaxed);
        }
        return Present1_hook.stdcall<HRESULT>(self, syncInterval, flags, parameters);
    }

    // The chain is sized to the window it is shown in (AR-014, second half). The engine
    // resizes its chain to its configured fullscreen resolution - the launcher's choice,
    // 3840x2160 here - whatever the window is: on a 1440x1080 desktop that was a 3840x2160
    // chain in a 1440x1080 borderless window, which DXGI stretches to fit, squeezing a 16:9
    // chain into a 4:3 window. With the chain at the window's client size the picture the
    // fit places in it lands on the screen 1:1.
    bool WindowClientSize(IDXGISwapChain* chain, UINT& width, UINT& height)
    {
        IDXGISwapChain1* one = nullptr;
        HWND window = nullptr;
        if (SUCCEEDED(chain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&one))) && one)
        {
            one->GetHwnd(&window);
            one->Release();
        }
        if (!window)
        {
            DXGI_SWAP_CHAIN_DESC desc {};
            if (SUCCEEDED(chain->GetDesc(&desc))) { window = desc.OutputWindow; }
        }
        RECT client {};
        if (!window || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) { return false; }
        width = static_cast<UINT>(client.right);
        height = static_cast<UINT>(client.bottom);
        return true;
    }

    void SizeToWindow(IDXGISwapChain* chain, UINT& width, UINT& height, const char* who)
    {
        if (!RenderPipeline::bOverrideWindowSize) { return; }   // stock play: leave the engine alone
        UINT w = 0, h = 0;
        if (WindowClientSize(chain, w, h) && (w != width || h != height))
        {
            spdlog::info("MGS4: fit: {} asked for {}x{}; the window is {}x{}, sizing the chain to it.", who, width, height, w, h);
            width = w;
            height = h;
        }
    }

    HRESULT STDMETHODCALLTYPE Hooked_ResizeBuffers(IDXGISwapChain* self, UINT bufferCount, UINT width,
        UINT height, DXGI_FORMAT format, UINT flags)
    {
        // Our back-buffer references would hold the old buffers and make the resize fail;
        // drop everything and rebuild lazily at the next Present that wants bands.
        {
            std::lock_guard lock(g_BandMutex);
            if (g_Band.swapChain) { BandRelease(g_Band); spdlog::info("MGS4: letterbox: swap chain resize to {}x{}, band renderer dropped.", width, height); }
        }
        SizeToWindow(self, width, height, "ResizeBuffers");
        return ResizeBuffers_hook.stdcall<HRESULT>(self, bufferCount, width, height, format, flags);
    }

    // The other resize entry point. The game's boot-time resize (a 2048x1536 chain grown to
    // the 3840x2160 desktop) went through here, unhooked, so our buffer references made it
    // fail and the small chain was stretched over the whole window (2026-09-04).
    HRESULT STDMETHODCALLTYPE Hooked_ResizeBuffers1(IDXGISwapChain3* self, UINT bufferCount, UINT width,
        UINT height, DXGI_FORMAT format, UINT flags, const UINT* nodeMask, IUnknown* const* presentQueue)
    {
        {
            std::lock_guard lock(g_BandMutex);
            if (g_Band.swapChain) { BandRelease(g_Band); spdlog::info("MGS4: letterbox: swap chain resize (ResizeBuffers1) to {}x{}, band renderer dropped.", width, height); }
        }
        SizeToWindow(self, width, height, "ResizeBuffers1");
        return ResizeBuffers1_hook.stdcall<HRESULT>(self, bufferCount, width, height, format, flags, nodeMask, presentQueue);
    }

    void HookSwapChain(IUnknown* presenter, IUnknown* swapChain)
    {
        static bool hooked = false;
        if (hooked || !swapChain) { return; }
        hooked = true;

        if (presenter)
        {
            ID3D12CommandQueue* queue = nullptr;
            if (SUCCEEDED(presenter->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))) && queue)
            {
                g_PresentQueue = queue;   // keeps its reference
                InstallFit12Device(queue, swapChain);
            }
        }
        void** vtable = *reinterpret_cast<void***>(swapChain);
        Present_hook = safetyhook::create_inline(vtable[kSwapChainPresentSlot], reinterpret_cast<void*>(Hooked_Present));
        ResizeBuffers_hook = safetyhook::create_inline(vtable[kSwapChainResizeBuffersSlot], reinterpret_cast<void*>(Hooked_ResizeBuffers));
        IDXGISwapChain1* one = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&one))) && one)
        {
            one->Release();
            Present1_hook = safetyhook::create_inline(vtable[kSwapChain1Present1Slot], reinterpret_cast<void*>(Hooked_Present1));
        }
        IDXGISwapChain3* three = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&three))) && three)
        {
            three->Release();
            ResizeBuffers1_hook = safetyhook::create_inline(vtable[kSwapChain3ResizeBuffers1Slot], reinterpret_cast<void*>(Hooked_ResizeBuffers1));
        }
        spdlog::info("MGS4: letterbox: swap chain hooked (Present {}, Present1 {}, ResizeBuffers {}, ResizeBuffers1 {}), queue {}.",
            Present_hook ? "ok" : "FAILED", Present1_hook ? "ok" : "absent", ResizeBuffers_hook ? "ok" : "FAILED",
            ResizeBuffers1_hook ? "ok" : "absent", g_PresentQueue ? "captured" : "MISSING");
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChain(IDXGIFactory* self, IUnknown* device,
        DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapChain)
    {
        const HRESULT result = CreateSwapChain_hook.stdcall<HRESULT>(self, device, desc, swapChain);
        if (SUCCEEDED(result) && swapChain && *swapChain) { HookSwapChain(device, *swapChain); }
        return result;
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateSwapChainForHwnd(IDXGIFactory2* self, IUnknown* device, HWND window,
        const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
        IDXGIOutput* restrictTo, IDXGISwapChain1** swapChain)
    {
        const HRESULT result = CreateSwapChainForHwnd_hook.stdcall<HRESULT>(self, device, window, desc, fullscreen, restrictTo, swapChain);
        if (SUCCEEDED(result) && swapChain && *swapChain) { HookSwapChain(device, *swapChain); }
        return result;
    }

    void HookFactory(IUnknown* factory)
    {
        static bool hooked = false;
        if (hooked || !factory) { return; }
        hooked = true;
        void** vtable = *reinterpret_cast<void***>(factory);
#if MGS4E_LAB_BUILD
        HookFactoryForCensus(factory);
#endif
        CreateSwapChain_hook = safetyhook::create_inline(vtable[kFactoryCreateSwapChainSlot], reinterpret_cast<void*>(Hooked_CreateSwapChain));
        IDXGIFactory2* two = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&two))) && two)
        {
            two->Release();
            CreateSwapChainForHwnd_hook = safetyhook::create_inline(vtable[kFactory2CreateSwapChainForHwndSlot], reinterpret_cast<void*>(Hooked_CreateSwapChainForHwnd));
        }
        spdlog::info("MGS4: letterbox: DXGI factory hooked (CreateSwapChain {}, CreateSwapChainForHwnd {}).",
            CreateSwapChain_hook ? "ok" : "FAILED", CreateSwapChainForHwnd_hook ? "ok" : "absent");
    }

    HRESULT WINAPI Hooked_CreateDXGIFactory(REFIID riid, void** factory)
    {
        const HRESULT result = CreateDXGIFactory_hook.stdcall<HRESULT>(riid, factory);
        if (SUCCEEDED(result) && factory && *factory) { HookFactory(static_cast<IUnknown*>(*factory)); }
        return result;
    }

    HRESULT WINAPI Hooked_CreateDXGIFactory1(REFIID riid, void** factory)
    {
        const HRESULT result = CreateDXGIFactory1_hook.stdcall<HRESULT>(riid, factory);
        if (SUCCEEDED(result) && factory && *factory) { HookFactory(static_cast<IUnknown*>(*factory)); }
        return result;
    }

    HRESULT WINAPI Hooked_CreateDXGIFactory2(UINT flags, REFIID riid, void** factory)
    {
        const HRESULT result = CreateDXGIFactory2_hook.stdcall<HRESULT>(flags, riid, factory);
        if (SUCCEEDED(result) && factory && *factory) { HookFactory(static_cast<IUnknown*>(*factory)); }
        return result;
    }

    // The camera hooks serve the masking's gate (the cinematic camera signal, the gameplay
    // camera for the ladder's fallbacks), the cutscene FOV compensation and the gameplay
    // FOV adjustment; the DXGI hooks serve the bands. Each half installs only when
    // something it serves is on.
    bool CameraHooksWanted()
    {
        return bMenuMasking || bCutsceneMasking || bCutsceneFovCompensation || fFovAdjustment != 1.0f;
    }

    bool BandHooksWanted()
    {
        // The fit (AR-014) rides on the same swap chain hooks and is needed whenever the
        // window size can differ from the desktop's.
        if (RenderPipeline::bOverrideWindowSize) { return true; }
#if MGS4E_LAB_BUILD
        // The perf census ticks from the Present hook, so a Lab build with Debug Logging on
        // hooks the chain even with the masking off; DrawBands then returns at once.
        if (mgs4e::log::Verbose()) { return true; }
#endif
        return bMenuMasking || bCutsceneMasking;
    }

    void InstallLetterbox()
    {
        auto* const base = reinterpret_cast<uint8_t*>(mgs4e::game::Module());
        const auto checked = [base](uintptr_t rva, std::initializer_list<uint8_t> prologue, const char* what) -> uint8_t*
            {
                auto* at = base + rva;
                if (!std::equal(prologue.begin(), prologue.end(), at))
                {
                    spdlog::error("MGS4: letterbox: {} at +{:X} does not start with the expected prologue - wrong executable build, not hooking.", what, rva);
                    return nullptr;
                }
                return at;
            };

#if MGS4E_LAB_BUILD
        InstallDisplayCensus();
#endif
        spdlog::info("MGS4: letterbox: menu masking {}, cutscene masking {}, cutscene FOV compensation {}, FOV adjustment {:.2f}.",
            bMenuMasking ? "on" : "off", bCutsceneMasking ? "on" : "off", bCutsceneFovCompensation ? "on" : "off", fFovAdjustment);
        g_CutsceneZoom.store(bCutsceneFovCompensation ? 1 : 0);
        if (!CameraHooksWanted() && !BandHooksWanted())
        {
            return;
        }

        // Camera side.
        if (CameraHooksWanted())
        {
        if (auto* at = checked(kRvaCameraBuilder, { 0x48, 0x8B, 0xC4, 0x53, 0x56, 0x57 }, "camera builder"))
        {
            CameraBuilder_hook = safetyhook::create_inline(at, reinterpret_cast<void*>(&BuildCamera));
            MGS4E_LOG_HOOK(CameraBuilder_hook, "MGS4: letterbox camera builder")
        }
        if (auto* at = checked(kRvaCinematicOwner, { 0x40, 0x55, 0x53, 0x57, 0x41, 0x56, 0x48, 0x8D }, "cinematic camera owner"))
        {
            CinematicOwner_hook = safetyhook::create_inline(at, reinterpret_cast<void*>(&UpdateCinematicCamera));
            MGS4E_LOG_HOOK(CinematicOwner_hook, "MGS4: letterbox cinematic owner")
        }
        if (auto* at = checked(kRvaProjectionSetter, { 0x48, 0x83, 0xEC, 0x68 }, "projection setter"))
        {
            ProjectionSetter_hook = safetyhook::create_inline(at, reinterpret_cast<void*>(&SetProjection));
            MGS4E_LOG_HOOK(ProjectionSetter_hook, "MGS4: letterbox projection setter")
        }
        }

        // Band side: the DXGI exports, before the game resolves them.
        if (!BandHooksWanted())
        {
            return;
        }
        const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        if (!dxgi) { spdlog::error("MGS4: letterbox: dxgi.dll did not load."); return; }
        if (void* fn = reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory")))
        {
            CreateDXGIFactory_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_CreateDXGIFactory));
        }
        if (void* fn = reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
        {
            CreateDXGIFactory1_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_CreateDXGIFactory1));
        }
        if (void* fn = reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory2")))
        {
            CreateDXGIFactory2_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_CreateDXGIFactory2));
        }
        spdlog::info("MGS4: letterbox: DXGI factory hooks: CreateDXGIFactory {}, CreateDXGIFactory1 {}, CreateDXGIFactory2 {}. Factor {:.4f} x {:.4f}.",
            CreateDXGIFactory_hook ? "ok" : "FAILED", CreateDXGIFactory1_hook ? "ok" : "FAILED", CreateDXGIFactory2_hook ? "ok" : "FAILED",
            HorizontalSpreadFactor(), VerticalSpreadFactor());
    }

    // ---- the HUD report (F11) --------------------------------------------------------------
    //
    // The one diagnostic a normal launch carries, behind the settings tool's Debug Logging: F11
    // writes the sites dump - every anchor record of the next instance-pool upload with its
    // owner, channel, node strcode and element offset - into the game log under a header
    // naming the output size and the active switches. It is exactly the dump the treatment
    // tables were built from, so a bug report with the log and a screenshot of the same
    // moment carries everything needed to table a widget that is still in the wrong place.
    void StartHudReportKey()
    {
        std::thread([]
            {
                bool wasDown = false;
                while (true)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    const bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
                    if (down && !wasDown)
                    {
                        int32_t outputWidth = 0, outputHeight = 0;
                        std::memcpy(&outputWidth, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kRvaWindowSizeX), sizeof(outputWidth));
                        std::memcpy(&outputHeight, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + kRvaWindowSizeY), sizeof(outputHeight));
                        spdlog::info("MGS4: HUD report (F11): output {}x{}, factor {:.4f} x {:.4f}, layout {}, world tracking {}.",
                            outputWidth, outputHeight, HorizontalSpreadFactor(), VerticalSpreadFactor(),
                            bAspectPoolSpread ? "expanded" : "centered", bAspectWorldTracking ? "on" : "off");
                        std::lock_guard lock(g_ObjectsMutex);
                        ArmSiteDump("HUD report");
                    }
                    wasDown = down;
                }
            }).detach();
        spdlog::info("MGS4: HUD report: F11 writes the HUD's anchor records to this log.");
    }

#if MGS4E_LAB_BUILD
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
                            // chanbias <channel> <dx> [dy]   (channel decimal or 0x.., deltas in NDC)
                            char* end = nullptr;
                            const long channel = std::strtol(line + 8, &end, 0);
                            const float dx = std::strtof(end, &end);
                            const float dy = std::strtof(end, nullptr);
                            if (channel >= 0 && channel < kChannelCount)
                            {
                                g_ChannelBias[channel] = dx;
                                g_ChannelBiasY[channel] = dy;
                                bool any = false;
                                for (float v : g_ChannelBias) { any = any || v != 0.0f; }
                                for (float v : g_ChannelBiasY) { any = any || v != 0.0f; }
                                g_ChannelBiasActive.store(any);
                                spdlog::info("MGS4: live lab: channel {} (0x{:X}) bias {:+.4f} {:+.4f}; active={}",
                                    channel, channel, dx, dy, any);
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
                            for (float& v : g_ChannelBiasY) { v = 0.0f; }
                            g_OrdChannel.store(-1);
                            g_ChannelBiasActive.store(false);
                            spdlog::info("MGS4: live lab: channel biases cleared.");
                        }
                        else if (std::strncmp(line, "sites", 5) == 0)
                        {
                            ArmSiteDump("live lab");
                        }
                        else if (std::strncmp(line, "drawlog", 7) == 0)
                        {
                            // drawlog N - log the next N command-list draws with their presents and callers.
                            const long n = std::strtol(line + 7, nullptr, 0);
                            g_DrawLogBudget.store(static_cast<int>(n > 0 ? n : 300));
                            spdlog::info("MGS4: live lab: draw census armed for {} draws (hooks {}).",
                                g_DrawLogBudget.load(), DrawInstanced_hook && DrawIndexedInstanced_hook ? "ok" : "MISSING");
                        }
                        else if (std::strncmp(line, "boxstat", 7) == 0)
                        {
                            std::lock_guard bandLock(g_BandMutex);
                            spdlog::info("MGS4: live lab: letterbox: mode {} zoom {} divisor {:.4f} patches {} presents {} cleared {} failed {} boot {} gameplay-cam {} ({} builds) plate-seen {} ms ago cutscene-live {} screen-live {} (last {:08X}, frames {}/{}) swapchain {}x{} painted {}x{} rects [{},{},{},{}] [{},{},{},{}] index {}.",
                                g_BandMode.load(), g_CutsceneZoom.load(), g_CutsceneDivisor.load(), g_CameraPatches.load(),
                                g_Band.presents, g_Band.cleared, g_Band.failed,
                                g_BootStage.load() == 0 ? "armed" : g_BootStage.load() == 1 ? "plate-up" : "over",
                                GameplayCameraLive() ? "yes" : "no", g_GameplayBuilds.load(),
                                g_PlateSeenTick.load() ? static_cast<long long>(GetTickCount64() - g_PlateSeenTick.load()) : -1LL,
                                CutsceneCameraLive() ? "yes" : "no",
                                ScreenLive() ? "yes" : "no", g_ScreenLastStrcode.load(),
                                g_ScreenInLastFrame.load() ? 1 : 0, g_ScreenInThisFrame.load() ? 1 : 0, g_Band.width, g_Band.height,
                                g_BandLastPainted[0], g_BandLastPainted[1],
                                g_BandLastRects[0].left, g_BandLastRects[0].top, g_BandLastRects[0].right, g_BandLastRects[0].bottom,
                                g_BandLastRects[1].left, g_BandLastRects[1].top, g_BandLastRects[1].right, g_BandLastRects[1].bottom,
                                g_BandLastIndex);
                        }
                        else if (std::strncmp(line, "box", 3) == 0)
                        {
                            // box 0|1|2 - bands cutscene-gated, forced on, forced off.
                            g_BandMode.store(static_cast<int>(std::strtol(line + 3, nullptr, 0)));
                            spdlog::info("MGS4: live lab: letterbox band mode {}.", g_BandMode.load());
                        }
                        else if (std::strncmp(line, "cutzoom", 7) == 0)
                        {
                            // cutzoom 0|1 - the cinematic camera zoom-out.
                            g_CutsceneZoom.store(static_cast<int>(std::strtol(line + 7, nullptr, 0)));
                            spdlog::info("MGS4: live lab: cutscene zoom {}.", g_CutsceneZoom.load() ? "on" : "off");
                        }
                        else if (std::strncmp(line, "cutdiv", 6) == 0)
                        {
                            // cutdiv <f> - explicit scale divisor for the cinematic camera; 0 = the spread factor.
                            g_CutsceneDivisor.store(std::strtof(line + 6, nullptr));
                            spdlog::info("MGS4: live lab: cutscene divisor {:.4f} (factor {:.4f}).", g_CutsceneDivisor.load(), HorizontalSpreadFactor());
                        }
                        else if (std::strncmp(line, "camlog", 6) == 0)
                        {
                            const int n = static_cast<int>(std::strtol(line + 6, nullptr, 10));
                            g_CameraLogBudget.store(n > 0 ? n : 60);
                            spdlog::info("MGS4: live lab: camera build log armed ({}).", g_CameraLogBudget.load());
                        }
                        else if (std::strncmp(line, "screenlog", 9) == 0)
                        {
                            const int n = static_cast<int>(std::strtol(line + 9, nullptr, 10));
                            g_ScreenLogAll.store(std::strstr(line + 9, "all") != nullptr);
                            g_ScreenLogCount = 0;
                            g_ScreenLogBudget.store(n > 0 ? n : 40);
                            spdlog::info("MGS4: live lab: screen identity log armed ({}{}).", g_ScreenLogBudget.load(),
                                g_ScreenLogAll.load() ? ", all HUD-owner nodes" : "");
                        }
                        else if (std::strncmp(line, "projlog", 7) == 0)
                        {
                            const int n = static_cast<int>(std::strtol(line + 7, nullptr, 10));
                            g_ProjectionLogBudget.store(n > 0 ? n : 60);
                            spdlog::info("MGS4: live lab: projection log armed ({}).", g_ProjectionLogBudget.load());
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
#endif

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
            reinterpret_cast<uint8_t*>(mgs4e::game::Module()) + kRvaUploadHelper, UploadHelperProbe);
        MGS4E_LOG_HOOK(UploadHelper_hook, "MGS4: UI Aspect producer watch")
    }

    void ApplyFixes()
    {
        // A normal launch gets the layout converter (layer 1, the undistorted centered
        // canvas), the pool append (layer 2, the identity-keyed corner spread; also the
        // masking's screen identities) and, for the masking and FOV settings, the camera
        // builder and DXGI hooks. Everything else here is research instrumentation - the
        // upload-helper probe, the live command thread, the rect/producer/channel logging -
        // whose switches only exist in the lab settings file and which must not run outside
        // a lab boot.
#if MGS4E_LAB_BUILD
        if (mgs4e::config::LabMode())
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
#endif

        // Menu masking without the corrected HUD would mask stretched menus: the settings tool
        // only offers it with a centered or expanded HUD, and the read enforces the same.
        bMenuMasking = bMenuMasking && bFixUiAspect;
        // The masking and the camera settings (AR-010) stand on their own: a stretched HUD
        // with masked, compensated cutscenes is a legitimate choice.
        const bool letterbox = CameraHooksWanted() || BandHooksWanted();
        if (!bFixUiAspect && !letterbox)
        {
            return;
        }

        if (bFixUiAspect)
        {
            auto* const at = reinterpret_cast<uint8_t*>(mgs4e::game::Module()) + kRvaUiLayoutConverter;

            // The RVA is specific to one executable build and .text is encrypted on disk, so
            // the only meaningful check is here, in-process, before hooking. MSVC's canonical
            // home-register prologue; anything else means a different binary.
            constexpr uint8_t kExpectedPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
            if (std::memcmp(at, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0)
            {
                spdlog::error("MGS4: UI Aspect: +{:X} does not start with the expected prologue - "
                    "wrong executable build, not hooking.", kRvaUiLayoutConverter);
                return;
            }

            UiLayoutConverter_hook = safetyhook::create_inline(at, reinterpret_cast<void*>(&ConvertLayout));
            MGS4E_LOG_HOOK(UiLayoutConverter_hook, "MGS4: UI Aspect")

            // Layer 2 rides on layer 1: without the undistorted centered base, spreading
            // anchors would just rearrange stretched widgets.
            if (!UiLayoutConverter_hook)
            {
                return;
            }
        }

#if MGS4E_LAB_BUILD
        if (mgs4e::config::LabMode())
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
#endif

        if (letterbox)
        {
            InstallLetterbox();
        }

        // Debug Logging: the F11 HUD report, which needs the upload probe (the dump runs at
        // the pool's upload - the lab has already installed it) as well as the append hook
        // below. A lab boot gets the key too; it arms the same dump as the lab's own command.
        const bool report = mgs4e::log::Verbose();
        if (report && !mgs4e::config::LabMode())
        {
            ApplyCornerSpread();
        }

        // The pool append hook, shared by the spread, the world tracking, the loading
        // spinner, the masking's screen identities and the HUD report. Its probe self-gates
        // to a flag check when nothing is armed, and every treatment is a strict no-op at
        // 16:9.
        if (bFixUiAspect || BandHooksWanted() || mgs4e::config::LabMode() || report)
        {
            InstallSiteAttribution();
        }

        if (report && PoolAppend_hook && UploadHelper_hook)
        {
            StartHudReportKey();
        }
    }
}
