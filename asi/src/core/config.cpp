#include "pch.hpp"
#include "config.hpp"

#include "game.hpp"
#include "log.hpp"
#include "ini.hpp"
#include "settings_keys.hpp"
#include "version.hpp"

#include "aspect_ratio.hpp"
#include "render_pipeline.hpp"
#if MGS4E_LAB_BUILD
#include "stage_automation.hpp"
#endif

namespace mgs4e::config
{
    namespace
    {
#if MGS4E_LAB_BUILD
        bool g_LabMode = false;
#endif
        std::filesystem::path g_File;

        // Reads a key into `out` when it is present and parses; otherwise leaves the default.
        // A present-but-malformed value is reported, since silently ignoring it would make
        // the user think the setting took.
        template<typename T>
        void Read(const mgs4e::Ini& ini, const char* section, const char* key, T& out)
        {
            const auto raw = ini.Raw(section, key);
            if (!raw)
            {
                return;
            }
            const auto value = mgs4e::Ini::Convert<T>(*raw);
            if (!value)
            {
                spdlog::warn("[{}] {}: could not read '{}', keeping the default.", section, key, *raw);
                return;
            }
            out = *value;
        }

        template<typename T>
        void Report(const char* section, const char* key, const T& value)
        {
            if (mgs4e::log::Verbose())
            {
                spdlog::info("Config: [{}] {} = {}", section, key, value);
            }
        }

        // Which file to read. A lab boot is signalled by the harness dropping MGS4Enabler.lab
        // next to a MGS4Enabler.lab.settings right before launch; if the marker is there and
        // fresh, that file is read instead and the research keys are honoured. The marker is
        // consumed here, so a lab boot cannot leak into the user's next normal launch, and the
        // user's own settings file is never rewritten or swapped. A marker older than fifteen
        // minutes is a leftover from a harness that died before the game came up: ignored and
        // cleared the same way.
        std::filesystem::path ChooseFile()
        {
            const std::filesystem::path root = mgs4e::game::Root();
            std::filesystem::path file = root / (std::string(MGS4E_NAME) + ".settings");

#if MGS4E_LAB_BUILD
            const std::filesystem::path marker = root / (std::string(MGS4E_NAME) + ".lab");
            std::error_code ec;
            if (std::filesystem::exists(marker, ec))
            {
                const auto written = std::filesystem::last_write_time(marker, ec);
                const bool fresh = !ec
                    && (std::filesystem::file_time_type::clock::now() - written) < std::chrono::minutes(15);
                const std::filesystem::path labFile = root / (std::string(MGS4E_NAME) + ".lab.settings");
                if (fresh && std::filesystem::exists(labFile, ec))
                {
                    g_LabMode = true;
                    file = labFile;
                }
                else
                {
                    spdlog::warn("Lab marker {} is {} - ignoring it.", marker.string(),
                        fresh ? "present but there is no " + labFile.filename().string() : "stale");
                }
                std::filesystem::remove(marker, ec);
            }
#endif
            return file;
        }

        void ReadWindowSize(const mgs4e::Ini& ini)
        {
            using namespace mgs4e::keys;

            // An aspect ratio picks which resolution key applies; the chosen entry is a
            // "WIDTHxHEIGHT" string.
            std::string aspect = WindowAspectRatio_Off;
            Read(ini, Graphics, WindowAspectRatio, aspect);

            std::string resolution;
            if (aspect == WindowAspectRatio_16_9)
            {
                Read(ini, Graphics, WindowResolution16x9, resolution);
            }
            else if (aspect == WindowAspectRatio_21_9)
            {
                Read(ini, Graphics, WindowResolution21x9, resolution);
            }
            else if (aspect == WindowAspectRatio_32_9)
            {
                Read(ini, Graphics, WindowResolution32x9, resolution);
            }
            else if (aspect == WindowAspectRatio_4_3)
            {
                Read(ini, Graphics, WindowResolution4x3, resolution);
            }

            if (!resolution.empty())
            {
                int width = 0;
                int height = 0;
                char separator = 0;
                std::istringstream parser(resolution);
                if ((parser >> width >> separator >> height) && (separator == 'x' || separator == 'X')
                    && width >= 320 && height >= 240 && width <= 16384 && height <= 16384)
                {
                    RenderPipeline::bOverrideWindowSize = true;
                    RenderPipeline::iWindowSizeX = width;
                    RenderPipeline::iWindowSizeY = height;
                }
                else
                {
                    spdlog::error("Could not read a resolution from '{}', leaving the game's window size alone.", resolution);
                }
            }

            Report(Graphics, WindowAspectRatio, aspect);
            if (RenderPipeline::bOverrideWindowSize)
            {
                Report(Graphics, "Window Resolution", resolution);
            }
        }

#if MGS4E_LAB_BUILD
        // The research keys, read only from the lab file. A normal launch ignores them even if
        // someone has copied them into the user's file, and a release build does not contain
        // this function or anything the keys switch on.
        void ReadLabKeys(const mgs4e::Ini& ini)
        {
            using namespace mgs4e::keys;
            namespace RP = RenderPipeline;

            Read(ini, Graphics, "Enable PIX Capture", RP::bEnablePixCapture);

            Read(ini, Graphics, "Redirect UI Coordinate Space", RP::bRedirectUiCoordinateSpace);
            Read(ini, Graphics, "UI NDC Site Start", RP::iUiNdcSiteStart);
            Read(ini, Graphics, "UI NDC Site Count", RP::iUiNdcSiteCount);
            Read(ini, Graphics, "UI Coordinate Probe", RP::bUiCoordinateProbe);
            Read(ini, Graphics, "Disassemble RVA", RP::sDisassembleRva);
            Read(ini, Graphics, "Disassemble Bytes", RP::iDisassembleBytes);
            Read(ini, Graphics, "Dump Floats", RP::sDumpFloats);
            Read(ini, Graphics, "Find String", RP::sFindString);
            Read(ini, Graphics, "Find Displacement", RP::sFindDisplacement);
            Read(ini, Graphics, "Scan Narrowing Conversions", RP::bScanNarrowingConversions);
            Read(ini, Graphics, "Scan Fixed Point Sites", RP::bScanFixedPointSites);
            Read(ini, Graphics, "Scan Cluster Disassembly", RP::bScanClusterDisassembly);
            Read(ini, Graphics, "Fixed Point Probe", RP::bFixedPointProbe);
            Read(ini, Graphics, "Breakpoint RVAs", RP::sBreakpointRvas);
            Read(ini, Graphics, "Watch Wrapped Value", RP::bWatchWrappedValue);
            Read(ini, Graphics, "Log Render Target Allocations", RP::bLogRenderTargetAllocations);
            Read(ini, Graphics, "Watch Render Size Reads", RP::bWatchRenderSizeReads);
            Read(ini, Graphics, "Watch Cached Render Size", RP::bWatchCachedRenderSize);
            Read(ini, Graphics, "Guard Wrapped Region", RP::bGuardWrappedRegion);
            Read(ini, Graphics, "Guard Record Heap Writes", RP::bGuardRecordHeapWrites);
            Read(ini, Graphics, "Trap Fixed Point Sites", RP::bTrapFixedPointSites);
            Read(ini, Graphics, "Dump Wrapped Context", RP::bDumpWrappedContext);
            Read(ini, Graphics, "Guard Upload Buffers", RP::bGuardUploadBuffers);
            Read(ini, Graphics, "Watch Staging Writes", RP::bWatchStagingWrites);

            // The engine's own fast-load path, used to cut the manual half of the test loop.
            Read(ini, Graphics, "Stage Automation", StageAutomation::bEnabled);
            Read(ini, Graphics, "Stage Automation Key", StageAutomation::iLoadStageKey);
            Read(ini, Graphics, "Stage Automation Stage", StageAutomation::sStage);
            Read(ini, Graphics, "Stage Automation Log Registry", StageAutomation::bLogStageRegistry);
            Read(ini, Graphics, "Stage Automation Auto Sequence", StageAutomation::bAutoSequence);
            Read(ini, Graphics, "Stage Automation Aim Hold (s)", StageAutomation::iAimHoldSeconds);
            Read(ini, Graphics, "Stage Automation Auto Start", StageAutomation::bAutoStartOnBoot);
            Read(ini, Graphics, "Stage Automation Exit After Run", StageAutomation::bExitAfterRun);

            Read(ini, Graphics, "Log Viewports", RP::bLogViewports);
            Read(ini, Graphics, "Log UI Layout", RP::bLogUiLayout);

            // The shipping "Ultrawide HUD" choice first, so a lab file written like a user file
            // gets the fix the way a user file does. Without this a lab boot silently ran with
            // the stretched HUD whenever the file had no per-layer keys (2026-09-04: three
            // README captures had to be retaken).
            {
                std::string hud;
                Read(ini, Graphics, UltrawideHud, hud);
                const bool expanded = hud == UltrawideHud_Expanded;
                const bool centered = hud == UltrawideHud_Centered;
                AspectRatio::bFixUiAspect = expanded || centered;
                AspectRatio::bAspectPoolSpread = expanded;
            }

            // The aspect layers individually, for the harness; these override the choice above.
            // Layer 1 is Fix UI Aspect, the corner spread is layer 2; the runtime anchoring is
            // the earlier, abandoned layer 2.
            Read(ini, Graphics, "Fix UI Aspect", AspectRatio::bFixUiAspect);
            Read(ini, Graphics, "Fix UI Aspect Corners", AspectRatio::bFixUiAspectCorners);
            Read(ini, Graphics, "Aspect Runtime Anchoring", AspectRatio::bAspectRuntimeAnchoring);
            Read(ini, Graphics, "Aspect Pool Spread", AspectRatio::bAspectPoolSpread);

            // The world-tracked label groups follow the pool spread unless the lab file says
            // otherwise, so older lab files keep their meaning.
            AspectRatio::bAspectWorldTracking = AspectRatio::bAspectPoolSpread;
            Read(ini, Graphics, "Aspect World Tracking", AspectRatio::bAspectWorldTracking);

            Read(ini, Graphics, "Watch Anchor Producers", RP::bWatchAnchorProducers);
            Read(ini, Graphics, "Log UI Channels", AspectRatio::bLogUiChannels);
            Read(ini, Graphics, "Log LA2 Loads", AspectRatio::bLogLa2Loads);
            Read(ini, Graphics, "Dump LA2", AspectRatio::bDumpLa2);

            // Hex strcode of one la2 whose layout object gets a visible test bias.
            {
                std::string strcode;
                Read(ini, Graphics, "Aspect Test Strcode", strcode);
                if (!strcode.empty())
                {
                    AspectRatio::uAspectTestStrcode = static_cast<uint32_t>(std::strtoul(strcode.c_str(), nullptr, 16));
                }
            }
            Read(ini, Graphics, "Aspect Test Ladder", AspectRatio::bAspectTestLadder);

            // The whole 16:9 masking at once (menus, cutscenes, the cutscene FOV compensation),
            // for the harness; the shipping keys then refine.
            bool letterbox = false;
            Read(ini, Graphics, "Aspect Letterbox", letterbox);
            AspectRatio::bMenuMasking = letterbox;
            AspectRatio::bCutsceneMasking = letterbox;
            AspectRatio::bCutsceneFovCompensation = letterbox;
            Read(ini, Graphics, MenuMasking, AspectRatio::bMenuMasking);
            Read(ini, Graphics, CutsceneMasking, AspectRatio::bCutsceneMasking);
            Read(ini, Graphics, CutsceneFovCompensation, AspectRatio::bCutsceneFovCompensation);

            int fov = 100;
            Read(ini, Graphics, FovAdjustment, fov);
            AspectRatio::fFovAdjustment = static_cast<float>(std::clamp(fov, 50, 200)) / 100.0f;
        }
#endif

        // The shipping aspect keys.
        void ReadAspectKeys(const mgs4e::Ini& ini)
        {
            using namespace mgs4e::keys;

            // Centered is layer 1 alone, Expanded is both layers. Read as text so an absent key
            // can be told apart from a chosen one: a settings file written before the option
            // existed leaves the HUD as the game draws it until the user sees the choice in the
            // tool. The on/off key the option started life as is honoured when the choice is
            // absent.
            std::string hud;
            Read(ini, Graphics, UltrawideHud, hud);
            if (hud.empty())
            {
                bool legacy = false;
                Read(ini, Graphics, UltrawideHudFix, legacy);
                hud = legacy ? UltrawideHud_Expanded : UltrawideHud_Stretched;
            }
            const bool expanded = hud == UltrawideHud_Expanded;
            const bool centered = hud == UltrawideHud_Centered;
            AspectRatio::bFixUiAspect = expanded || centered;
            AspectRatio::bAspectPoolSpread = expanded;
            Report(Graphics, UltrawideHud, hud);

            // Rides on either corrected layout; inert with a stretched one, where the labels
            // were never moved off their targets.
            bool solidEye = true;
            Read(ini, Graphics, SolidEyeOverlayFix, solidEye);
            AspectRatio::bAspectWorldTracking = AspectRatio::bFixUiAspect && solidEye;
            Report(Graphics, SolidEyeOverlayFix, solidEye);

            // The 16:9 masking. Menu masking rides on a corrected layout like the overlay fix
            // (masking stretched menus would cut them off); the cutscene switches stand on their
            // own. All three default on: a settings file written before they existed gets the
            // authored framing, which is what the aspect options exist for. Every one is a no-op
            // at 16:9, and a window explicitly set to a 16:9 size skips the hooks altogether;
            // "Use Game Setting" keeps them, since the display may be wide.
            bool menuMasking = true;
            bool cutsceneMasking = true;
            bool cutsceneFov = true;
            Read(ini, Graphics, MenuMasking, menuMasking);
            Read(ini, Graphics, CutsceneMasking, cutsceneMasking);
            Read(ini, Graphics, CutsceneFovCompensation, cutsceneFov);
            Report(Graphics, MenuMasking, menuMasking);
            Report(Graphics, CutsceneMasking, cutsceneMasking);
            Report(Graphics, CutsceneFovCompensation, cutsceneFov);
            const bool sixteenNine = RenderPipeline::bOverrideWindowSize
                && RenderPipeline::iWindowSizeX * 9 == RenderPipeline::iWindowSizeY * 16;
            AspectRatio::bMenuMasking = AspectRatio::bFixUiAspect && menuMasking && !sixteenNine;
            AspectRatio::bCutsceneMasking = cutsceneMasking && !sixteenNine;
            AspectRatio::bCutsceneFovCompensation = cutsceneFov && !sixteenNine;

            // The gameplay camera's view width as a percentage of stock; 100 is a no-op.
            int fov = 100;
            Read(ini, Graphics, FovAdjustment, fov);
            fov = std::clamp(fov, 50, 200);
            AspectRatio::fFovAdjustment = static_cast<float>(fov) / 100.0f;
            Report(Graphics, FovAdjustment, fov);
        }

        void ReadGraphicsKeys(const mgs4e::Ini& ini)
        {
            using namespace mgs4e::keys;
            namespace RP = RenderPipeline;

            // Up to 4x. The buffer width clamp is what actually limits this - 400% of a 1080p
            // output is 7680 wide, while 400% of 4K would be 15360 and gets cut to the 8K
            // ceiling. Capping the percentage lower would make 8K unreachable from anything but
            // a 4K output, which is backwards: supersampling matters most on smaller outputs.
            int internalScale = 100;
            Read(ini, Graphics, InternalResolutionScale, internalScale);
            internalScale = std::clamp(internalScale, 100, 400);
            RP::fInternalResolutionScale = internalScale / 100.0;
            Report(Graphics, InternalResolutionScale, internalScale);

            // Undocumented: removes the 16-bit truncation that loses the reticle above a
            // 4095-wide buffer. On by default because it is a strict improvement on the old
            // shader workaround, but overridable while it is still being validated.
            Read(ini, Graphics, FixReticleTruncation, RP::bFixReticleTruncation);

            // Undocumented escape hatch for probing where the crosshair actually breaks.
            Read(ini, Graphics, MaxInternalBufferWidth, RP::iMaxInternalBufferWidth);
            RP::iMaxInternalBufferWidth = std::clamp(RP::iMaxInternalBufferWidth, 640, 8192);

            // Read as text rather than straight into a bool, so an absent key can be told apart
            // from a false one. A settings file written before this option existed must leave
            // the game's own FXAA choice alone rather than forcing it on.
            std::string fxaa;
            Read(ini, Graphics, Fxaa, fxaa);
            if (!fxaa.empty())
            {
                if (const auto on = mgs4e::Ini::Convert<bool>(fxaa))
                {
                    RP::iFxaaOverride = *on ? 1 : 0;
                    Report(Graphics, Fxaa, *on);
                }
            }

            std::string fxaaQuality;
            Read(ini, Graphics, FxaaQuality, fxaaQuality);
            if (!fxaaQuality.empty())
            {
                RP::iFxaaQuality =
                    (fxaaQuality == FxaaQuality_Slow) ? 0 :
                    (fxaaQuality == FxaaQuality_Medium) ? 1 :
                    (fxaaQuality == FxaaQuality_Fast) ? 2 : -1;
                Report(Graphics, FxaaQuality, fxaaQuality);
            }

            Read(ini, Graphics, ShadowSampleCount, RP::iShadowSampleCount);
            RP::iShadowSampleCount = std::clamp(RP::iShadowSampleCount, 0, 32);
            Report(Graphics, ShadowSampleCount, RP::iShadowSampleCount);

            Read(ini, Graphics, AnisotropicFiltering, RP::iAnisotropicFiltering);
            RP::iAnisotropicFiltering = std::clamp(RP::iAnisotropicFiltering, 0, 16);
            Report(Graphics, AnisotropicFiltering, RP::iAnisotropicFiltering);

            int shadowScale = 100;
            Read(ini, Graphics, ShadowResolutionScale, shadowScale);
            shadowScale = std::clamp(shadowScale, 100, 400);
            RP::fShadowResolutionScale = shadowScale / 100.0;
            Report(Graphics, ShadowResolutionScale, shadowScale);
        }
    }

    void Load()
    {
        g_File = ChooseFile();

        mgs4e::Ini ini;
        if (!ini.Load(g_File))
        {
            spdlog::warn("No settings file at {} - running with defaults. Run {}.exe from the game folder to create one.",
                g_File.string(), MGS4E_DISPLAY_NAME);
            return;
        }

        spdlog::info("Settings file: {}", g_File.string());
#if MGS4E_LAB_BUILD
        if (g_LabMode)
        {
            spdlog::warn("LAB MODE: research instrumentation enabled for this boot only.");
        }
#endif

        bool verbose = false;
        Read(ini, mgs4e::keys::Debugging, mgs4e::keys::DebugLogging, verbose);
        mgs4e::log::SetVerbose(verbose);
        Report(mgs4e::keys::Debugging, mgs4e::keys::DebugLogging, verbose);

        ReadWindowSize(ini);
        ReadGraphicsKeys(ini);
#if MGS4E_LAB_BUILD
        if (g_LabMode)
        {
            ReadLabKeys(ini);
            return;
        }
#endif
        ReadAspectKeys(ini);
    }

#if MGS4E_LAB_BUILD
    bool LabMode()
    {
        return g_LabMode;
    }
#endif

    const std::filesystem::path& File()
    {
        return g_File;
    }
}
