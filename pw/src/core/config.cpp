#include "pch.hpp"
#include <algorithm>
#include "config.hpp"

#include "game.hpp"
#include "log.hpp"
#include "ini.hpp"
#include "version.hpp"
#include "pw/settings_keys.hpp"

#include "internal_size.hpp"
#if MGS4E_LAB_BUILD
#include "probe.hpp"
#include "draw_census.hpp"
#endif

namespace mgspwe::config
{
    namespace
    {
#if MGS4E_LAB_BUILD
        bool g_LabMode = false;
#endif
        std::filesystem::path g_File;

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

        // Which file to read: the same marker protocol as the MGS4 build (its config.cpp),
        // with this build's names. The harness drops MGSPWEnabler.lab next to
        // MGSPWEnabler.lab.settings right before launch; a fresh marker selects that file and
        // is consumed here, so a lab boot never leaks into the user's next normal launch and
        // the user's own file is never rewritten. A marker older than fifteen minutes is a
        // leftover from a harness that died before the game came up: ignored and cleared.
        std::filesystem::path ChooseFile()
        {
            const std::filesystem::path root = mgs4e::game::Root();
            std::filesystem::path file = root / (std::string(MGSPWE_NAME) + ".settings");

#if MGS4E_LAB_BUILD
            const std::filesystem::path marker = root / (std::string(MGSPWE_NAME) + ".lab");
            std::error_code ec;
            if (std::filesystem::exists(marker, ec))
            {
                const auto written = std::filesystem::last_write_time(marker, ec);
                const bool fresh = !ec
                    && (std::filesystem::file_time_type::clock::now() - written) < std::chrono::minutes(15);
                const std::filesystem::path labFile = root / (std::string(MGSPWE_NAME) + ".lab.settings");
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

        // Research keys (shared/pw/settings_keys.hpp) are read only by a Lab build; the window's
        // keys (the shape, the resolutions of that shape, the window mode, MSAA, filtering) by both.
        // "WxH" or "WxH (..." -> width, height; false when the text is not that.
        bool ParseSize(const std::string& text, int& w, int& h)
        {
            int a = 0, b = 0;
            if (sscanf_s(text.c_str(), "%dx%d", &a, &b) != 2 || a <= 0 || b <= 0) { return false; }
            w = a; h = b;
            return true;
        }

        // The window's settings: aspect ratio, the resolution lists of the selected shape, the
        // window mode and MSAA, mapped onto the switches the Lab build runs on. Files written
        // before the window had these keys keep their old meaning.
        void ReadWindowKeys(const mgs4e::Ini& ini)
        {
            using namespace mgspwe::keys;
            struct Shape { const char* name; int w, h; const char* screenKey; const char* renderKey; };
            static const Shape kShapes[] = {
                { AspectRatio_16_9,  480, 272, ScreenResolution16x9,  RenderResolution16x9 },
                { AspectRatio_16_10, 480, 300, ScreenResolution16x10, RenderResolution16x10 },
                { AspectRatio_21_9,  650, 272, ScreenResolution21x9,  RenderResolution21x9 },
                { AspectRatio_32_9,  967, 272, ScreenResolution32x9,  RenderResolution32x9 },
                { AspectRatio_4_3,   480, 360, ScreenResolution4x3,   RenderResolution4x3 },
            };
            const auto aspect = ini.Raw(Graphics, AspectRatio);
            if (aspect)
            {
                const Shape* shape = nullptr;
                for (const Shape& sh : kShapes) { if (*aspect == sh.name) { shape = &sh; } }
                if (shape)
                {
                    InternalSize::bWideCanvas = !(shape->w == 480 && shape->h == 272);
                    InternalSize::iWideCanvasUnits = shape->w;
                    InternalSize::iWideCanvasHeightUnits = shape->h;
                    int w = 0, h = 0;
                    if (const auto screen = ini.Raw(Graphics, shape->screenKey); screen && ParseSize(*screen, w, h))
                    {
                        InternalSize::iOutputWidth = w; InternalSize::iOutputHeight = h;
                    }
                    if (const auto render = ini.Raw(Graphics, shape->renderKey); render && ParseSize(*render, w, h))
                    {
                        InternalSize::iRenderScale = std::max(1, w / shape->w);
                    }
                    spdlog::info("Config: [Graphics] {} = {} -> canvas {}x{}, output {}x{}, render scale {}.", AspectRatio, *aspect,
                        shape->w, shape->h, InternalSize::iOutputWidth, InternalSize::iOutputHeight, InternalSize::iRenderScale);
                }
                else if (*aspect == AspectRatio_Display)
                {
                    // The display's shape: the canvas and the output resolve at start-up.
                    InternalSize::bWideCanvas = true;
                    InternalSize::iWideCanvasUnits = 0; InternalSize::iWideCanvasHeightUnits = 0;
                    InternalSize::iOutputWidth = 0; InternalSize::iOutputHeight = 0;
                    Read(ini, Graphics, RenderScale, InternalSize::iRenderScale);
                    spdlog::info("Config: [Graphics] {} = {} -> from the display, render scale {}.", AspectRatio, *aspect, InternalSize::iRenderScale);
                }
                else { spdlog::warn("[Graphics] {}: '{}' is not a shape the window offers; the display's shape is used.", AspectRatio, *aspect); InternalSize::bWideCanvas = true; }
            }
            if (const auto mode = ini.Raw(Graphics, WindowMode))
            {
                if (*mode == WindowMode_Off) { InternalSize::iWindowMode = -1; }
                else if (*mode == WindowMode_Fullscreen) { InternalSize::iWindowMode = 2; }
                else if (*mode == WindowMode_Borderless) { InternalSize::iWindowMode = 0; }
                else if (*mode == WindowMode_Windowed) { InternalSize::iWindowMode = 1; }
                else { Read(ini, Graphics, WindowMode, InternalSize::iWindowMode); }   // a Lab file's number
                Report(Graphics, WindowMode, InternalSize::iWindowMode);
            }
            if (const auto msaa = ini.Raw(Graphics, Msaa))
            {
                InternalSize::bDisableMsaa = (*msaa == Msaa_Off);
                Report(Graphics, Msaa, *msaa);
            }
            if (const auto af = ini.Raw(Graphics, AnisotropicFiltering))
            {
                // A checkbox in the tool (true = 16x); a number from a hand-edited file is taken as the level.
                if (*af == "true") { InternalSize::iAnisotropy = 16; }
                else if (*af == "false") { InternalSize::iAnisotropy = 0; }
                else { Read(ini, Graphics, AnisotropicFiltering, InternalSize::iAnisotropy); }
                Report(Graphics, AnisotropicFiltering, InternalSize::iAnisotropy);
            }
            Read(ini, Graphics, GpuLocalTextures, InternalSize::bGpuLocalTextures);
            Report(Graphics, GpuLocalTextures, InternalSize::bGpuLocalTextures);
            Read(ini, Graphics, FullscreenRefreshFix, InternalSize::bFullscreenRefreshFix);
            Report(Graphics, FullscreenRefreshFix, InternalSize::bFullscreenRefreshFix);
            Read(ini, Graphics, FullscreenResolutionFix, InternalSize::bFullscreenResolutionFix);
            Report(Graphics, FullscreenResolutionFix, InternalSize::bFullscreenResolutionFix);
        }

#if MGS4E_LAB_BUILD
        void ReadLabKeys(const mgs4e::Ini& ini)
        {
            using namespace mgspwe::keys;
            Read(ini, Graphics, LogLoadedModules, Probe::bLogLoadedModules);
            Read(ini, Graphics, LogDecryptTiming, Probe::bLogDecryptTiming);
            Read(ini, Graphics, LogCommandLine, Probe::bLogCommandLine);
            Report(Graphics, LogLoadedModules, Probe::bLogLoadedModules);
            Report(Graphics, LogDecryptTiming, Probe::bLogDecryptTiming);
            Report(Graphics, LogCommandLine, Probe::bLogCommandLine);
            Read(ini, Graphics, DrawCensusAtSeconds, DrawCensus::iCensusAtSeconds);
            Read(ini, Graphics, DrawCensusFrames, DrawCensus::iCensusFrames);
            Read(ini, Graphics, LiveCommands, DrawCensus::bLiveCommands);
            Read(ini, Graphics, TimeFrames, DrawCensus::iTimeFrames);
            Read(ini, Graphics, TimePasses, DrawCensus::bTimePasses);
            Read(ini, Graphics, DumpShaders, DrawCensus::bDumpShaders);
            Read(ini, Graphics, LightHooks, DrawCensus::bLightHooks);
            Report(Graphics, LightHooks, DrawCensus::bLightHooks);
            Read(ini, Graphics, PacingLog, DrawCensus::bPacingLog);
            Report(Graphics, PacingLog, DrawCensus::bPacingLog);
            Read(ini, Graphics, HitchSampler, DrawCensus::bHitchSampler);
            Report(Graphics, HitchSampler, DrawCensus::bHitchSampler);
            Read(ini, Graphics, VBlankWaitTimeout, DrawCensus::iVBlankWaitTimeout);
            Report(Graphics, VBlankWaitTimeout, DrawCensus::iVBlankWaitTimeout);
            Read(ini, Graphics, FrameHandoffWait, DrawCensus::iFrameHandoffWait);
            Report(Graphics, FrameHandoffWait, DrawCensus::iFrameHandoffWait);
            Read(ini, Graphics, VBlankWaitLog, DrawCensus::bVBlankWaitLog);
            Report(Graphics, VBlankWaitLog, DrawCensus::bVBlankWaitLog);
            Read(ini, Graphics, FrameSkipGovernor, DrawCensus::bFrameSkipGovernor);
            Report(Graphics, FrameSkipGovernor, DrawCensus::bFrameSkipGovernor);
            {
                std::string pacing = "Game";
                Read(ini, Graphics, FramePacing, pacing);
                DrawCensus::iFramePacing = (_stricmp(pacing.c_str(), "Display") == 0) ? 1 : (_stricmp(pacing.c_str(), "VBlank") == 0) ? 2 : 0;
                Report(Graphics, FramePacing, DrawCensus::iFramePacing == 1 ? "Display" : DrawCensus::iFramePacing == 2 ? "VBlank" : "Game");
            }
            Read(ini, Graphics, PresentSync, DrawCensus::iPresentSync);
            Report(Graphics, PresentSync, DrawCensus::iPresentSync);
            Read(ini, Graphics, AllowTearing, DrawCensus::bAllowTearing);
            Report(Graphics, AllowTearing, DrawCensus::bAllowTearing);
            Read(ini, Graphics, SwapChainBuffers, InternalSize::iSwapChainBuffers);
            Report(Graphics, SwapChainBuffers, InternalSize::iSwapChainBuffers);
            Report(Graphics, DrawCensusAtSeconds, DrawCensus::iCensusAtSeconds);
            Report(Graphics, DrawCensusFrames, DrawCensus::iCensusFrames);
            Report(Graphics, LiveCommands, DrawCensus::bLiveCommands);
            Read(ini, Graphics, RenderScale, InternalSize::iRenderScale);
            Read(ini, Graphics, RenderScaleHundredths, InternalSize::iRenderScaleHundredths);
            Read(ini, Graphics, WideCanvas, InternalSize::bWideCanvas);
            Report(Graphics, WideCanvas, InternalSize::bWideCanvas);
            Read(ini, Graphics, WideCanvasUnits, InternalSize::iWideCanvasUnits);
            Report(Graphics, WideCanvasUnits, InternalSize::iWideCanvasUnits);
            Read(ini, Graphics, WideCanvasHeightUnits, InternalSize::iWideCanvasHeightUnits);
            Report(Graphics, WideCanvasHeightUnits, InternalSize::iWideCanvasHeightUnits);
            Read(ini, Graphics, SceneSizeWidth, InternalSize::iSceneWidth);
            Read(ini, Graphics, SceneSizeHeight, InternalSize::iSceneHeight);
            Report(Graphics, SceneSizeWidth, InternalSize::iSceneWidth);
            Report(Graphics, SceneSizeHeight, InternalSize::iSceneHeight);
            Report(Graphics, RenderScaleHundredths, InternalSize::iRenderScaleHundredths);
            Read(ini, Graphics, InternalSizeWidth, InternalSize::iInternalWidth);
            Read(ini, Graphics, InternalSizeHeight, InternalSize::iInternalHeight);
            Read(ini, Graphics, OutputSizeWidth, InternalSize::iOutputWidth);
            Read(ini, Graphics, OutputSizeHeight, InternalSize::iOutputHeight);
            Read(ini, Graphics, DisableMsaa, InternalSize::bDisableMsaa);
            Read(ini, Graphics, GpuLocalTextures, InternalSize::bGpuLocalTextures);
            Report(Graphics, GpuLocalTextures, InternalSize::bGpuLocalTextures);
            Read(ini, Graphics, FullscreenRefreshFix, InternalSize::bFullscreenRefreshFix);
            Report(Graphics, FullscreenRefreshFix, InternalSize::bFullscreenRefreshFix);
            Read(ini, Graphics, FullscreenResolutionFix, InternalSize::bFullscreenResolutionFix);
            Report(Graphics, FullscreenResolutionFix, InternalSize::bFullscreenResolutionFix);
            Read(ini, Graphics, BackBufferAtInternalSize, InternalSize::bBackBufferAtInternal);
            Report(Graphics, BackBufferAtInternalSize, InternalSize::bBackBufferAtInternal);
            Report(Graphics, DisableMsaa, InternalSize::bDisableMsaa);
            Report(Graphics, RenderScale, InternalSize::iRenderScale);
            Report(Graphics, InternalSizeWidth, InternalSize::iInternalWidth);
            Report(Graphics, InternalSizeHeight, InternalSize::iInternalHeight);
            Report(Graphics, OutputSizeWidth, InternalSize::iOutputWidth);
            Report(Graphics, OutputSizeHeight, InternalSize::iOutputHeight);
        }
#endif
    }

    void Load()
    {
        g_File = ChooseFile();

        mgs4e::Ini ini;
        if (!ini.Load(g_File))
        {
            spdlog::warn("No settings file at {} - running with defaults. Run the settings tool from the game folder to create one.",
                g_File.string());
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
        Read(ini, mgspwe::keys::Debugging, mgspwe::keys::DebugLogging, verbose);
        mgs4e::log::SetVerbose(verbose);
        Report(mgspwe::keys::Debugging, mgspwe::keys::DebugLogging, verbose);

#if MGS4E_LAB_BUILD
        // A Lab build reads the research knobs from whichever file it loaded: the user's own
        // settings (the tool's Peace Walker window edits them) or, with the marker, the lab file.
        ReadLabKeys(ini);
#endif
        ReadWindowKeys(ini);
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
