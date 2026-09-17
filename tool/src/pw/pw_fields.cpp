#include "pch.hpp"
#include "games.hpp"

#include "pw/settings_keys.hpp"
#include "pw/identity.hpp"

// The Peace Walker settings the tool edits: the window (aspect ratio, the resolution lists of the
// selected shape, window mode, rendering), the Lab page a Lab build of the ASI reads (docs/pw/lab.md)
// and the logging switch.
namespace mgs4e::tool::pw
{
    namespace
    {
        namespace K = mgspwe::keys;
        using F = Field;

        constexpr const char* kHelp_DebugLogging =
            "Writes more detail to logs\\" MGSPWE_NAME "_Game.log: which hooks were installed and what each setting resolved to.\n"
            "\n"
            "Turn it on before reproducing a problem, then attach the log to your report.";

        constexpr const char* kHelp_RenderScale =
            "The scene renders at this integer multiple of the PSP's 480x272 canvas; the game's own launcher\n"
            "settings are 3 (1440x816) and 4 (1920x1088). 8 is 3840x2176 (4K wide), 12 is 5760x3264, 16 is\n"
            "7680x4352 (8K wide). The game's own final blit downscales into the window; no oversized window.\n"
            "0 leaves the game's value.\n"
            "\n"
            "Needs a Lab build of " MGSPWE_NAME ".asi in mgspw\\scripts; a Release build ignores this tab.";
        constexpr const char* kHelp_OutputSize =
            "The picture the game fits into the window (its output table). Set it to the window size to fill a\n"
            "16:9 window, or to a 16:9 picture that fits the window height on a wider display (the game then\n"
            "pillarboxes it). 0 leaves the game's table.";
        constexpr const char* kHelp_InternalSize =
            "The post-processing chain's target size, when it should differ from the scene. 0 = same as the\n"
            "scene (the only configuration measured to be a win so far).";
        constexpr const char* kHelp_DisableMsaa =
            "Creates the scene targets single-sampled and turns the game's resolve into a copy. About 2 ms at 8K.\n"
            "The game's own resolve shaders are written for 4 samples; check aiming and depth of field with it on.";
        constexpr const char* kHelp_GpuLocal =
            "The game's full-size frame textures carry CPU write access and end up in system memory, so its\n"
            "per-frame copies into them cross PCIe (5 ms each at 8K). This keeps them in video memory. The game\n"
            "was never seen to write them from the CPU; the log reports it if it ever does.";
        constexpr const char* kHelp_WideCanvas =
            "The infrastructure for displays that are not 16:9: the canvas is widened to the display's aspect\n"
            "(650 PSP units at 21:9), the world fills it, the HUD and menus stay at the 16:9 scale and are\n"
            "centred, and the full-screen effects are widened with it. Off = a plain 16:9 run, pillarboxed.\n"
            "Narrower than 16:9 makes the canvas taller instead (480x360 at 4:3, 480x300 at 16:10), with the world\n"
            "above and below the UI. Units: the canvas width and height in PSP units; 0 takes them from the\n"
            "primary display. With Wide Canvas on and Output Size 0, the output becomes the display size. For a\n"
            "4:3 or 16:10 desktop mode use Window Mode 1 (windowed picks a 16:9 preset, exclusive the native mode).";
        constexpr const char* kHelp_WindowMode =
            "What the game is told its saved display mode is: 0 windowed, 1 fullscreen window, 2 exclusive\n"
            "fullscreen (the in-game Options value, kept in the save folder). Exclusive fullscreen takes the\n"
            "display mode Windows offers: on a monitor whose native resolution is 16:9 that is the native mode,\n"
            "not a 21:9 desktop resolution, so pin 0 there; on a native 21:9 output exclusive mode works with\n"
            "the wide canvas. -1 leaves the saved value. The saved values go to the log.";
        constexpr const char* kHelp_BackBuffer =
            "Experiment: the swap chain's buffers at the scene size, stretched onto the window by the compositor\n"
            "instead of the game's blit. Measured to cost about the same as the game's own blit.";
        constexpr const char* kHelp_Census =
            "Draw census: logs every draw of N frames (targets, shaders, textures, callers) to the game log, at\n"
            "the given time after start. 0 = only on command. Use 2 frames: the first is always empty.";
        constexpr const char* kHelp_Live =
            "Polls C:\\mgspf_tools\\pw\\live.txt for commands and F11 for a census plus the timing run.";
        constexpr const char* kHelp_Timing =
            "GPU timestamps around the passes of a census frame, averaged over this many frames (one in four),\n"
            "reported in the game log with the frame rate over the window.";
        constexpr const char* kHelp_DumpShaders =
            "Writes every shader's bytecode to C:\\mgspf_tools\\pw\\shaders for disassembly.";
        constexpr const char* kHelp_LightHooks =
            "Leaves the game's draws unhooked (only the resolve is hooked): the frame rate of a Release-like\n"
            "build with the rendering knobs still applied. No census or timing in this mode.";
        constexpr const char* kHelp_BusyWaitFix =
            "The game never waits: it spins while it should be idle, which costs about a whole processor "
            "core and is what caused the stutter at 60 Hz.\n\n"
            "Off leaves the game as it shipped. Render thread is the fix that matters and is the safest. "
            "Render thread and ticker adds the frame timer. Everything also lets the window sleep between "
            "messages, which is the only part that touches other programs, so turn it down a step if an "
            "overlay misbehaves.";

        constexpr const char* kHelp_AudioPrefetch =
            "EXPERIMENTAL, off by default. Whenever a new voice line starts, the game reads it from a large archive on the main thread and "
            "waits for the disk. On a cold archive that wait was about ten milliseconds, which is the single "
            "frame hitch at the start of a Codec call or a voice line.\n\n"
            "Voice archives at start reads them through in the background right after launch, about 1.6 GB, "
            "so they are already in memory when the game asks. Windows gives that memory back the moment "
            "anything else needs it. As opened warms each archive only once the game has opened it, which "
            "does not help the first line. Off leaves the game as it shipped. Try Voice archives at start if you see a single-frame hitch when Codec calls or voice lines begin.";

        constexpr const char* kHelp_StateObjectCache =
            "The game asks the graphics card to build the same few hundred render settings every single "
            "frame, when it only ever uses about forty of them. They never change once built, so this "
            "hands back the one it asked for last time.\n\n"
            "Saves roughly one frame's worth of processor work in twelve. Worth most on a slower "
            "processor. Turn it off only if you are chasing a rendering problem.";

        constexpr const char* kHelp_FullscreenRefreshFix =
            "On (the normal state): exclusive Fullscreen asks for the display's current refresh rate. The game asks\n"
            "for 60 Hz whatever the display runs, which on a 120 Hz desktop dropped one frame a second. Off: the\n"
            "game's request stands, for comparison runs.";
        constexpr const char* kHelp_FullscreenResolutionFix =
            "On (the normal state): exclusive Fullscreen runs at the selected Screen Resolution. The game picks the\n"
            "largest mode the monitor advertises (4096x2160 on some 4K panels). Off: the game's pick stands.";
        constexpr const char* kHelp_PacingLog =
            "Frame pacing instrumentation: hooks the game's Sleep and wait imports (every one of the render thread's\n"
            "~170,000 Sleep(0) calls a frame passes through the hook, a measurable cost), counts the 60 Hz ticker, and\n"
            "logs a 5 s pacing summary (frames, ticks, Present time). Off for judging runs.";
        constexpr const char* kHelp_HitchSampler =
            "When the render thread has waited 20 ms for the game thread's frame, every other thread is sampled once:\n"
            "module and offset, the game return addresses on its stack, and the wait it sits in. Up to 12 samples a run,\n"
            "as 'PW sampler:' blocks in the log. A light hook on Sleep; fine for judging runs.";
        constexpr const char* kHelp_VBlankWaitTimeout =
            "The game thread waits for the 60 Hz tick on a shared event that two threads reset; a wakeup lost in that\n"
            "race costs a whole frame (the doubled frame every second or so at 60 Hz). 0 leaves the game's infinite\n"
            "wait; 1 gives the wait a 1 ms timeout so a lost wakeup costs a millisecond.";
        constexpr const char* kHelp_FrameHandoffWait =
            "The game drops a finished frame when its render thread still holds the previous one (inside a vsync-blocked\n"
            "Present), and the display repeats the last frame: the doubled frame every second or so at 60 Hz in Borderless\n"
            "and Fullscreen. N waits up to N ms for the render thread instead. 0 leaves the game's drop.";
        constexpr const char* kHelp_VBlankWaitLog =
            "Logs the game thread's vblank waits: how many 60 Hz ticks passed between two waits and how long its work\n"
            "took in between. Every skipped tick (a doubled frame) is logged with both, plus a 5 s summary per thread.";
        constexpr const char* kHelp_FrameSkipGovernor =
            "On: the game's own behaviour. It measures each frame in vblanks and, when one measures even slightly long,\n"
            "waits two vblanks for the next frame (a doubled frame), then recovers. In Borderless and Fullscreen at 60 Hz\n"
            "the render thread's Present lands frame ends right on the vblank, so this trips about once a second: the\n"
            "frame-time spike. Off: the count is never raised by the governor; the game's explicit 30 fps setting for\n"
            "movies and menus is untouched.";
        constexpr const char* kHelp_FramePacing =
            "The game's 60 Hz ticker thread. Game: as shipped, 60.000 Hz by the CPU clock. Display: the same clock at\n"
            "the display's exact refresh rate. VBlank: the thread waits for the display's vertical blank (one per tick at\n"
            "60 Hz, two at 120 Hz) so tick and display never drift apart. Displays that are not a multiple of 60 Hz keep Game.";
        constexpr const char* kHelp_PresentSync =
            "-1 leaves the game's Present sync interval (1, a vsync wait). 0..4 forces the interval.";
        constexpr const char* kHelp_SwapChainBuffers =
            "0 leaves the game's flip-model swap chain (2 buffers). 3 gives Present a frame of slack, at a frame of latency.";
        constexpr const char* kHelp_AllowTearing =
            "Creates the swap chain tearing-capable and presents windowed frames with sync 0 and the tearing flag, so Present\n"
            "never waits for the display. Borderless stays whole through the compositor; exclusive Fullscreen would tear.";
        constexpr const char* kHelp_Probe =
            "One-off log lines at start-up: the loaded modules, the code decryption timing, the command line.";

        constexpr const char* kHelp_AspectRatio =
            "The shape of the picture. 16:9 is the game's own layout. Any other shape widens or heightens the\n"
            "PSP canvas to it (650x272 units at 21:9, 480x360 at 4:3): the world fills the display at correct\n"
            "proportions, the HUD and menus keep the 16:9 scale and stay centred, the full-screen effects follow,\n"
            "and the HUD groups move to the edges with the 16:9 margin. Use Display takes the shape of your\n"
            "primary display at start-up.";
        constexpr const char* kHelp_ScreenResolution =
            "The size of the picture on screen: the window's client area, or the display mode in Fullscreen. Only\n"
            "the list of the selected shape is used. With Use Display the picture is the display's size.";
        constexpr const char* kHelp_RenderResolution =
            "The size the scene is rendered at, before the game's own downscale into the picture: an integer\n"
            "multiple of the canvas (the game's launcher offers 3x and 4x). Higher is supersampling; 8x is fine on\n"
            "a 4090, 16x needs the frame textures kept in video memory. Only the list of the selected shape is used.";
        constexpr const char* kHelp_RenderScaleDisplay =
            "With Use Display the canvas is not known until start-up, so the render size is given as the integer\n"
            "multiple of the canvas: 4 = the game's Full HD setting, 8, 10, 12, 16.";
        constexpr const char* kHelp_Anisotropy =
            "Anisotropic filtering on every texture the game samples with linear filtering (its own samplers use\n"
            "none). 16 is free on any modern card and keeps ground and wall textures sharp at grazing angles.\n"
            "Samplers the game sets to point filtering are left alone. 0 = the game's own.";
        constexpr const char* kHelp_Msaa =
            "The game renders the scene with 4x MSAA. Off creates the scene targets single-sampled and turns the\n"
            "game's resolve into a copy: about 2 ms saved at 8K. The game's resolve shaders are written for 4\n"
            "samples; check aiming and depth of field with it off.";
        constexpr const char* kHelp_GpuLocalRelease =
            "The game's full-size frame textures carry CPU write access and end up in system memory, so its\n"
            "per-frame copies into them cross PCIe (5 ms each at 8K). On, they stay in video memory: 60 fps at 8K\n"
            "instead of 27. The game was never seen to write them from the CPU; the log reports it if it ever does.";
        constexpr const char* kHelp_WindowModeRelease =
            "Fullscreen is the game's exclusive mode (it takes the display mode Windows offers: on a monitor whose\n"
            "native resolution is 16:9 that is the native mode, not a 21:9 desktop resolution), Borderless a\n"
            "frameless window the size of the display, Windowed a normal window with the selected Screen\n"
            "Resolution as its client area. The choice is applied every launch and shows in the in-game Options,\n"
            "which can still change it for the run. Use Game Setting keeps the value saved by the game.";

        std::vector<Page> BuildPages()
        {
            const char* G = K::Graphics;
            const char* D = K::Debugging;
            return {
                { "Graphics", {
                    { "Window", {
                        F::Choice(G, K::AspectRatio, kHelp_AspectRatio, K::AspectRatio_16_9,
                            { K::AspectRatio_Display, K::AspectRatio_16_9, K::AspectRatio_16_10, K::AspectRatio_21_9, K::AspectRatio_32_9, K::AspectRatio_4_3 }),

                        // One screen list per shape, shown only while its shape is selected (the MGS4 window's pattern).
                        F::Choice(G, K::ScreenResolution16x9, kHelp_ScreenResolution, "1920x1080",
                            { "1280x720", "1366x768", "1600x900", "1920x1080", "2560x1440", "2880x1620", "3200x1800", "3840x2160", "5120x2880", "7680x4320" })
                            .ShownWhen(K::AspectRatio, { K::AspectRatio_16_9 }),
                        F::Choice(G, K::ScreenResolution16x10, kHelp_ScreenResolution, "1920x1200",
                            { "1280x800", "1440x900", "1680x1050", "1920x1200", "2560x1600", "2880x1800", "3840x2400" })
                            .ShownWhen(K::AspectRatio, { K::AspectRatio_16_10 }),
                        F::Choice(G, K::ScreenResolution21x9, kHelp_ScreenResolution, "3440x1440",
                            { "2560x1080", "2560x1088", "3440x1440", "3440x1600", "3840x1600", "3840x1620", "5120x2160" })
                            .ShownWhen(K::AspectRatio, { K::AspectRatio_21_9 }),
                        F::Choice(G, K::ScreenResolution32x9, kHelp_ScreenResolution, "5120x1440",
                            { "3840x1080", "3840x1200", "5120x1440", "5120x1600", "7680x2160" })
                            .ShownWhen(K::AspectRatio, { K::AspectRatio_32_9 }),
                        F::Choice(G, K::ScreenResolution4x3, kHelp_ScreenResolution, "1024x768",
                            { "640x480", "800x600", "1024x768", "1152x864", "1280x960", "1400x1050", "1440x1080", "1600x1200", "1920x1440", "2048x1536", "2560x1920", "2880x2160", "3200x2400" })
                            .ShownWhen(K::AspectRatio, { K::AspectRatio_4_3 }),

                        F::Choice(G, K::WindowMode, kHelp_WindowModeRelease, K::WindowMode_Off,
                            { K::WindowMode_Off, K::WindowMode_Fullscreen, K::WindowMode_Borderless, K::WindowMode_Windowed }),
                    }},

                    { "Rendering", {
                        // One render list per shape: the canvas of that shape times the integer scales.
                        F::Choice(G, K::RenderResolution16x9, kHelp_RenderResolution, "1920x1088 (4x)",
                            { "1440x816 (3x)", "1920x1088 (4x)", "2880x1632 (6x)", "3840x2176 (8x)", "4800x2720 (10x)", "5760x3264 (12x)", "7680x4352 (16x)" })
                            .ShownWhen(K::AspectRatio, { K::AspectRatio_16_9 }),
                        F::Choice(G, K::RenderResolution16x10, kHelp_RenderResolution, "1920x1200 (4x)",
                            { "1440x900 (3x)", "1920x1200 (4x)", "2880x1800 (6x)", "3840x2400 (8x)", "4800x3000 (10x)", "5760x3600 (12x)", "7680x4800 (16x)" })
                            .ShownWhen(K::AspectRatio, { K::AspectRatio_16_10 }),
                        F::Choice(G, K::RenderResolution21x9, kHelp_RenderResolution, "2600x1088 (4x)",
                            { "1950x816 (3x)", "2600x1088 (4x)", "3900x1632 (6x)", "5200x2176 (8x)", "6500x2720 (10x)", "7800x3264 (12x)", "10400x4352 (16x)" })
                            .ShownWhen(K::AspectRatio, { K::AspectRatio_21_9 }),
                        F::Choice(G, K::RenderResolution32x9, kHelp_RenderResolution, "3868x1088 (4x)",
                            { "2901x816 (3x)", "3868x1088 (4x)", "5802x1632 (6x)", "7736x2176 (8x)", "9670x2720 (10x)", "11604x3264 (12x)" })
                            .ShownWhen(K::AspectRatio, { K::AspectRatio_32_9 }),
                        F::Choice(G, K::RenderResolution4x3, kHelp_RenderResolution, "1920x1440 (4x)",
                            { "1440x1080 (3x)", "1920x1440 (4x)", "2880x2160 (6x)", "3840x2880 (8x)", "4800x3600 (10x)", "5760x4320 (12x)", "7680x5760 (16x)" })
                            .ShownWhen(K::AspectRatio, { K::AspectRatio_4_3 }),
                        F::Int(G, K::RenderScale, kHelp_RenderScaleDisplay, 4, 1, 24)
                            .ShownWhen(K::AspectRatio, { K::AspectRatio_Display }),

                        F::Choice(G, K::Msaa, kHelp_Msaa, K::Msaa_4x, { K::Msaa_4x, K::Msaa_Off }),
                        F::Bool(G, K::AnisotropicFiltering, kHelp_Anisotropy, true),
                        F::Bool(G, K::GpuLocalTextures, kHelp_GpuLocalRelease, true),
                    }},
                    { "Performance", {
                        F::Choice(G, K::BusyWaitFix, kHelp_BusyWaitFix, "Everything",
                            { "Off", "Render thread", "Render thread and ticker", "Everything" }),
                        F::Bool(G, K::StateObjectCache, kHelp_StateObjectCache, true),
                        F::Choice(G, K::AudioPrefetch, kHelp_AudioPrefetch, K::AudioPrefetch_Off,
                            { K::AudioPrefetch_Off, K::AudioPrefetch_OnOpen, K::AudioPrefetch_All }),
                    }},
                }},
                { "Lab", {
                    { "Fullscreen fixes (both builds)", {
                        F::Bool(G, K::FullscreenRefreshFix, kHelp_FullscreenRefreshFix, true),
                        F::Bool(G, K::FullscreenResolutionFix, kHelp_FullscreenResolutionFix, true),
                    }},
                    { "Frame pacing experiments", {
                        F::Bool(G, K::PacingLog, kHelp_PacingLog, false),
                        F::Bool(G, K::HitchSampler, kHelp_HitchSampler, false),
                        F::Int(G, K::VBlankWaitTimeout, kHelp_VBlankWaitTimeout, 0, 0, 16),
                        F::Int(G, K::FrameHandoffWait, kHelp_FrameHandoffWait, 0, 0, 16),
                        F::Bool(G, K::VBlankWaitLog, kHelp_VBlankWaitLog, false),
                        F::Bool(G, K::FrameSkipGovernor, kHelp_FrameSkipGovernor, true),
                        F::Choice(G, K::FramePacing, kHelp_FramePacing, K::FramePacing_Game, { K::FramePacing_Game, K::FramePacing_Display, K::FramePacing_VBlank }),
                        F::Int(G, K::PresentSync, kHelp_PresentSync, -1, -1, 4),
                        F::Int(G, K::SwapChainBuffers, kHelp_SwapChainBuffers, 0, 0, 8),
                        F::Bool(G, K::AllowTearing, kHelp_AllowTearing, false),
                    }},
                    { "Draw census", {
                        F::Int(G, K::DrawCensusAtSeconds, kHelp_Census, 0, 0, 3600),
                        F::Int(G, K::DrawCensusFrames, kHelp_Census, 2, 1, 10),
                        F::Bool(G, K::LiveCommands, kHelp_Live, true),
                    }},
                    { "GPU timing", {
                        F::Bool(G, K::TimePasses, kHelp_Timing, true),
                        F::Int(G, K::TimeFrames, kHelp_Timing, 60, 1, 600),
                    }},
                    { "Experiments (0 = off)", {
                        F::Int(G, K::InternalSizeWidth, kHelp_InternalSize, 0, 0, 16384),
                        F::Int(G, K::InternalSizeHeight, kHelp_InternalSize, 0, 0, 16384),
                        F::Bool(G, K::BackBufferAtInternalSize, kHelp_BackBuffer, false),
                    }},
                    { "Other", {
                        F::Bool(G, K::DumpShaders, kHelp_DumpShaders, false),
                        F::Bool(G, K::LightHooks, kHelp_LightHooks, false),
                        F::Bool(G, K::LogDecryptTiming, kHelp_Probe, false),
                        F::Bool(G, K::LogCommandLine, kHelp_Probe, false),
                        F::Bool(G, K::LogLoadedModules, kHelp_Probe, false),
                    }},
                }},
                { "Troubleshooting", {
                    { "Logging", {
                        F::Bool(D, K::DebugLogging, kHelp_DebugLogging, false),
                    }},
                }},
            };
        }
    }

    const std::vector<Page>& Pages()
    {
        static const std::vector<Page> pages = BuildPages();
        return pages;
    }

    const std::vector<const Field*>& AllFields()
    {
        static const std::vector<const Field*> all = []
        {
            std::vector<const Field*> v;
            for (const Page& p : Pages())
            {
                for (const Group& g : p.groups)
                {
                    for (const Field& f : g.fields)
                    {
                        v.push_back(&f);
                    }
                }
            }
            return v;
        }();
        return all;
    }

    const Field* FindField(const std::string& section, const std::string& key)
    {
        for (const Field* f : AllFields())
        {
            if (section == f->section && key == f->key)
            {
                return f;
            }
        }
        return nullptr;
    }
}
